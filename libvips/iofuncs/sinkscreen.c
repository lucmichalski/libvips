/* aynchronous screen sink
 *
 * 1/1/10
 * 	- from im_render.c
 */

/*

    This file is part of VIPS.
    
    VIPS is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

 */

/*

    These files are distributed with VIPS - http://www.vips.ecs.soton.ac.uk

 */

/* Verbose debugging output.
#define VIPS_DEBUG
 */

/* Trace allocate/free.
#define VIPS_DEBUG_RED
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <vips/intl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /*HAVE_UNISTD_H*/

#include <vips/vips.h>
#include <vips/thread.h>
#include <vips/debug.h>

#ifdef WITH_DMALLOC
#include <dmalloc.h>
#endif /*WITH_DMALLOC*/

/* A have-threads we can test in if().
 */
#ifdef HAVE_THREADS
static const int have_threads = 1;
#else /*!HAVE_THREADS*/
static const int have_threads = 0;
#endif /*HAVE_THREADS*/

/* A tile in our cache. 
 */
typedef struct {
	struct _Render *render;

	Rect area;		/* Place here (unclipped) */
	gboolean painted;	/* Tile contains valid pixels (ie. not dirty) */
	REGION *region;		/* REGION with the pixels */

	int ticks;		/* Time of last use, for LRU flush */
} Tile;

/* Per-call state.
 */
typedef struct _Render {
	/* Reference count this, since we use these things from several
	 * threads. We can't easily use the gobject ref count system since we
	 * need a lock around operations.
	 */
	int ref_count;
	GMutex *ref_count_lock;	

	/* Parameters.
	 */
	VipsImage *in;		/* Image we render */
	VipsImage *out;		/* Write tiles here on demand */
	VipsImage *mask;	/* Set valid pixels here */
	int tile_width;		/* Tile size */
	int tile_height;
	int max_tiles;		/* Maximum number of tiles */
	int priority;		/* Larger numbers done sooner */
	VipsSinkNotify notify;	/* Tell caller about paints here */
	void *a;

	/* Lock here before reading or modifying the tile structure. 
	 */
	GMutex *lock;	

	/* Tile cache.
	 */
	GSList *all;		/* All our tiles */
	int ntiles;		/* Number of tiles */
	int ticks;		/* Inc. on each access ... used for LRU */

	/* List of dirty tiles. Most recent at the front.
	 */
	GSList *dirty;		

	/* Hash of tiles with positions. Tiles can be dirty or painted.
	 */
	GHashTable *tiles;
} Render;

/* Our per-thread state.
 */
typedef struct _RenderThreadState {
	VipsThreadState parent_object;

	/* The tile that should be calculated.
	 */
	Tile *tile;
} RenderThreadState;

typedef struct _RenderThreadStateClass {
	VipsThreadStateClass parent_class;

} RenderThreadStateClass;

G_DEFINE_TYPE( RenderThreadState, render_thread_state, VIPS_TYPE_THREAD_STATE );

/* The BG thread which sits waiting to do some calculations.
 */
static GThread *render_thread = NULL;

/* Number of renders with dirty tiles. render_thread queues up on this.
 */
static im_semaphore_t render_dirty_sem;

/* All the renders with dirty tiles.
 */
static GMutex *render_dirty_lock = NULL;
static GSList *render_dirty_all = NULL;

/* Set this to make the bg thread stop and reschedule.
 */
static gboolean render_reschedule = FALSE;

static void
render_thread_state_class_init( RenderThreadStateClass *class )
{
	VipsObjectClass *object_class = VIPS_OBJECT_CLASS( class );

	object_class->nickname = "renderthreadstate";
	object_class->description = _( "per-thread state for render" );
}

static void
render_thread_state_init( RenderThreadState *state )
{
	state->tile = NULL;
}

static VipsThreadState *
render_thread_state_new( VipsImage *im, void *a )
{
	return( VIPS_THREAD_STATE( vips_object_new( 
		render_thread_state_get_type(), 
		vips_thread_state_set, im, a ) ) );
}

static void *
tile_free( Tile *tile )
{
	VIPS_DEBUG_MSG_RED( "tile_free\n" );

	IM_FREEF( im_region_free, tile->region );
	im_free( tile );

	return( NULL );
}

static int
render_free( Render *render )
{
	VIPS_DEBUG_MSG_RED( "render_free: %p\n", render );

	g_assert( render->ref_count == 0 );

	g_mutex_lock( render_dirty_lock );
	if( g_slist_find( render_dirty_all, render ) ) {
		render_dirty_all = g_slist_remove( render_dirty_all, render );
		im_semaphore_upn( &render_dirty_sem, -1 );
	}
	g_mutex_unlock( render_dirty_lock );

	g_mutex_free( render->ref_count_lock );
	g_mutex_free( render->lock );

	im_slist_map2( render->all, (VSListMap2Fn) tile_free, NULL, NULL );
	IM_FREEF( g_slist_free, render->all );
	render->ntiles = 0;
	IM_FREEF( g_slist_free, render->dirty );
	IM_FREEF( g_hash_table_destroy, render->tiles );

	im_free( render );

	return( 0 );
}

/* Ref and unref a Render ... free on last unref.
 */
static int
render_ref( Render *render )
{
	g_mutex_lock( render->ref_count_lock );
	g_assert( render->ref_count != 0 );
	render->ref_count += 1;
	g_mutex_unlock( render->ref_count_lock );

	return( 0 );
}

static int
render_unref( Render *render )
{
	int kill;

	g_mutex_lock( render->ref_count_lock );
	g_assert( render->ref_count > 0 );
	render->ref_count -= 1;
	kill = render->ref_count == 0;
	g_mutex_unlock( render->ref_count_lock );

	if( kill )
		render_free( render );

	return( 0 );
}

/* Wait for a render with dirty tiles. 
 */
static Render *
render_dirty_get( void )
{
	Render *render;

	/* Wait for a render with dirty tiles.
	 */
	im_semaphore_down( &render_dirty_sem );

	g_mutex_lock( render_dirty_lock );

	/* Just take the head of the jobs list ... we sort when we add. If
	 * render_free() is called between our semaphore letting us in
	 * and the _lock(), render_dirty_all can be NULL.
	 */
	render = NULL;
	if( render_dirty_all ) {
		render = (Render *) render_dirty_all->data;

		/* Ref the render to make sure it can't die while we're
		 * working on it.
		 */
		render_ref( render );

		render_dirty_all = g_slist_remove( render_dirty_all, render );
	}

	g_mutex_unlock( render_dirty_lock );

	return( render );
}

static int 
render_allocate( VipsThreadState *state, void *a, gboolean *stop )
{
	Render *render = (Render *) a;
	RenderThreadState *rstate = (RenderThreadState *) state;

	g_mutex_lock( render->lock );

	if( render_reschedule || !render->dirty ) {
		*stop = TRUE;
		rstate->tile = NULL;
	}
	else {
		Tile *tile;

		tile = (Tile *) render->dirty->data;
		render->dirty = g_slist_remove( render->dirty, tile );
		rstate->tile = tile;
	}

	g_mutex_unlock( render->lock );

	return( 0 );
}

static int 
render_work( VipsThreadState *state, void *a )
{
	Render *render = (Render *) a;
	RenderThreadState *rstate = (RenderThreadState *) state;
	Tile *tile = rstate->tile;

	g_assert( tile );

	if( !tile->painted ) { 
		VIPS_DEBUG_MSG( "calculating tile %dx%d\n", 
			tile->area.left, tile->area.top );

		if( im_prepare_to( state->reg, tile->region, 
			&tile->area, tile->area.left, tile->area.top ) ) 
			return( -1 );
		tile->painted = TRUE;

		/* Now clients can update.
		 */
		if( render->notify ) 
			render->notify( render->out, &tile->area, render->a );
	}

	return( 0 );
}

static int       
render_dirty_sort( Render *a, Render *b )
{
	return( b->priority - a->priority );
}

/* Add to the jobs list, if it has work to be done.
 */
static void
render_dirty_put( Render *render )
{
	g_mutex_lock( render_dirty_lock );

	if( render->dirty ) {
		if( !g_slist_find( render_dirty_all, render ) ) {
			render_dirty_all = g_slist_prepend( render_dirty_all, 
				render );
			render_dirty_all = g_slist_sort( render_dirty_all,
				(GCompareFunc) render_dirty_sort );

			/* Ask the bg thread to stop and reschedule, if it's
			 * running.
			 */
			render_reschedule = TRUE;

			im_semaphore_up( &render_dirty_sem );
		}
	}

	g_mutex_unlock( render_dirty_lock );
}

/* Main loop for RenderThreads.
 */
static void *
render_thread_main( void *client )
{
	for(;;) {
		Render *render;

		if( (render = render_dirty_get()) ) {
			/* Ignore errors, I'm not sure what we'd do with them
			 * anyway.
			 */
			render_reschedule = FALSE;
			(void) vips_threadpool_run( render->in,
				render_thread_state_new,
				render_allocate,
				render_work,
				NULL,
				render );

			/* Add back to the jobs list, if we need to.
			 */
			render_dirty_put( render );

			/* _get() does a ref to make sure we keep the render
			 * alive during processing ... unref before we loop.
			 * This can kill off the render.
			 */
			render_unref( render );
		}
	}

	return( NULL );
}

/* Create our set of RenderThread. Assume we're single-threaded here.
 */
static int
render_thread_create( void )
{
	if( !have_threads )
		return( 0 );

	if( !render_dirty_lock ) {
		render_dirty_lock = g_mutex_new();
		im_semaphore_init( &render_dirty_sem, 0, "render_dirty_sem" );
	}

	if( !render_thread && have_threads ) {
		if( !(render_thread = g_thread_create_full( 
			render_thread_main, NULL, 
			IM__DEFAULT_STACK_SIZE, TRUE, FALSE, 
			G_THREAD_PRIORITY_NORMAL, NULL )) ) {
			im_error( "im_render", 
				"%s", _( "unable to create thread" ) );
			return( -1 );
		}
	}

	return( 0 );
}

static guint
tile_hash( gconstpointer key )
{
	Rect *rect = (Rect *) key;

	int x = rect->left / rect->width;
	int y = rect->top / rect->height;

	return( x << 16 ^ y );
}

static gboolean
tile_equal( gconstpointer a, gconstpointer b )
{
	Rect *rect1 = (Rect *) a;
	Rect *rect2 = (Rect *) b;

	return( rect1->left == rect2->left &&
		rect1->top == rect2->top );
}

static int
render_close_cb( Render *render )
{
	render_unref( render );

	/* If this render is being worked on, we want to jog the bg thread, 
	 * make it drop it's ref and think again.
	 */
	render_reschedule = TRUE;

	return( 0 );
}

static Render *
render_new( VipsImage *in, VipsImage *out, VipsImage *mask, 
	int tile_width, int tile_height, 
	int max_tiles, 
	int priority,
	VipsSinkNotify notify, void *a )
{
	Render *render;

	/* Don't use auto-free for render, we do our own lifetime management
	 * with _ref() and _unref().
	 */
	if( !(render = IM_NEW( NULL, Render )) )
		return( NULL );

	render->ref_count = 1;
	render->ref_count_lock = g_mutex_new();

	render->in = in;
	render->out = out;
	render->mask = mask;
	render->tile_width = tile_width;
	render->tile_height = tile_height;
	render->max_tiles = max_tiles;
	render->priority = priority;
	render->notify = notify;
	render->a = a;

	render->lock = g_mutex_new();

	render->all = NULL;
	render->ntiles = 0;
	render->ticks = 0;

	render->tiles = g_hash_table_new( tile_hash, tile_equal ); 

	render->dirty = NULL;

	/* Both out and mask must close before we can free the render.
	 */

	if( im_add_close_callback( out, 
                (im_callback_fn) render_close_cb, render, NULL ) ) {
                (void) render_unref( render );
                return( NULL );
        }

	if( mask ) {
		if( im_add_close_callback( mask, 
			(im_callback_fn) render_close_cb, render, NULL ) ) {
			(void) render_unref( render );
			return( NULL );
		}
		render_ref( render );
	}

	return( render );
}

/* Make a Tile.
 */
static Tile *
tile_new( Render *render )
{
	Tile *tile;

	VIPS_DEBUG_MSG_RED( "tile_new\n" );

	/* Don't use auto-free: we need to make sure we free the tile after
	 * Render.
	 */
	if( !(tile = IM_NEW( NULL, Tile )) )
		return( NULL );

	tile->render = render;
	tile->region = NULL;
	tile->area.left = 0;
	tile->area.top = 0;
	tile->area.width = 0;
	tile->area.height = 0;
	tile->ticks = render->ticks;

	if( !(tile->region = im_region_create( render->in )) ) {
		(void) tile_free( tile );
		return( NULL );
	}

	render->all = g_slist_prepend( render->all, tile );
	render->ntiles += 1;

	return( tile );
}

/* Search the cache for a tile by position.
 */
static Tile *
render_tile_lookup( Render *render, Rect *area )
{
	return( (Tile *) g_hash_table_lookup( render->tiles, area ) );
}

/* We've looked at a tile ... bump to end of LRU and front of dirty.
 */
static void
tile_touch( Tile *tile )
{
	Render *render = tile->render;

	tile->ticks = render->ticks;
	render->ticks += 1;

	if( !tile->painted ) {
		VIPS_DEBUG_MSG( "tile_bump_dirty: bumping tile %dx%d\n",
			tile->area.left, tile->area.top );

		if( g_slist_find( render->dirty, tile ) ) {
			render->dirty = g_slist_remove( render->dirty, tile );
			render->dirty = g_slist_prepend( render->dirty, tile );
		}
	}
}

/* Queue a tile for calculation. It might need moving too.
 */
static void
tile_queue( Tile *tile, Rect *area )
{
	Render *render = tile->render;

	VIPS_DEBUG_MSG( "tile_set_dirty: adding tile %dx%d to dirty\n",
		area->left, area->top );

	tile->painted = FALSE;
	tile->area = *area;
	if( im_region_buffer( tile->region, area ) )
		printf( "poop!\n" );
	g_hash_table_insert( render->tiles, &tile->area, tile );

	if( render->notify && have_threads ) {
		/* Add to the list of renders with dirty tiles. The bg 
		 * thread will pick it up and paint it.
		 */
		render->dirty = g_slist_prepend( render->dirty, tile );
		render_dirty_put( render );
	}
	else {
		/* No threads, or no notify ... paint the tile ourselves 
		 * sychronously. No need to notify the client since they'll 
		 * never see black tiles.
		 */
		VIPS_DEBUG_MSG( "tile_set_dirty: "
			"painting tile %dx%d synchronously\n",
			area->left, area->top );

		im_prepare( tile->region, &tile->area );
		tile->painted = TRUE;
	}
}

static void 
tile_test_clean_ticks( Rect *key, Tile *value, Tile **best )
{
	if( value->painted )
		if( !*best || value->ticks < (*best)->ticks )
			*best = value;
}

/* Pick a painted tile to reuse. Search for LRU (slow!).
 */
static Tile *
render_tile_get_painted( Render *render )
{
	Tile *tile;

	tile = NULL;
	g_hash_table_foreach( render->tiles,
		(GHFunc) tile_test_clean_ticks, &tile );

	if( tile ) {
		g_assert( tile->painted );

		VIPS_DEBUG_MSG( "render_tile_get_painted: "
			"reusing painted %p\n", tile );
	}

	return( tile );
}

/* Pick a dirty tile to reuse. We could potentially get the tile that
 * render_work() is working on in the background :-( but I don't think we'll
 * get a crash, just a mis-paint. It should be vanishingly impossible anyway.
 */
static Tile *
render_tile_get_dirty( Render *render )
{
	Tile *tile;

	if( !render->dirty )
		tile = NULL;
	else {
		tile = (Tile *) g_slist_last( render->dirty )->data;
		render->dirty = g_slist_remove( render->dirty, tile );
	}

	return( tile );
}

/* Ask for an area of calculated pixels. Get from cache, request calculation,
 * or if we've no threads or no notify, calculate immediately.
 */
static Tile *
tile_request( Render *render, Rect *area )
{
	Tile *tile;

	if( (tile = render_tile_lookup( render, area )) ) {
		/* We already have a tile at this position. If it's invalid,
		 * ask for a repaint.
		 */
		if( !tile->painted || tile->region->invalid ) 
			tile_queue( tile, area );
	}
	else if( render->ntiles < render->max_tiles || 
		render->max_tiles == -1 ) {
		/* We have fewer tiles than teh max. We can just make a new 
		 * tile.
		 */
		if( !(tile = tile_new( render )) ) 
			return( NULL );

		tile_queue( tile, area );
	}
	else {
		/* Need to reuse a tile. Try for an old painted tile first, 
		 * then if that fails, reuse a dirty tile. 
		 */
		if( !(tile = render_tile_get_painted( render )) &&
			!(tile = render_tile_get_dirty( render )) ) 
			return( NULL );

		VIPS_DEBUG_MSG( "(render_tile_get: was at %dx%d, "
			"moving to %dx%d)\n",
			tile->area.left, tile->area.top,
			area->left, area->top );

		/* Need to remove from the old position.
		 */
		g_hash_table_remove( render->tiles, &tile->area );

		tile_queue( tile, area );
	}

	tile_touch( tile );

	return( tile );
}

/* Copy what we can from the tile into the region.
 */
static void
tile_copy( Tile *tile, REGION *to )
{
	Rect ovlap;
	int y;

	/* Find common pixels.
	 */
	im_rect_intersectrect( &tile->area, &to->valid, &ovlap );
	g_assert( !im_rect_isempty( &ovlap ) );

	/* If the tile is painted, copy over the pixels. Otherwise, fill with
	 * zero. 
	 */
	if( tile->painted && !tile->region->invalid ) {
		int len = IM_IMAGE_SIZEOF_PEL( to->im ) * ovlap.width;

		VIPS_DEBUG_MSG( "tile_copy: "
			"copying calculated pixels for %dx%d\n",
			tile->area.left, tile->area.top ); 

		for( y = ovlap.top; y < IM_RECT_BOTTOM( &ovlap ); y++ ) {
			PEL *p = (PEL *) IM_REGION_ADDR( tile->region, 
				ovlap.left, y );
			PEL *q = (PEL *) IM_REGION_ADDR( to, ovlap.left, y );

			memcpy( q, p, len );
		}
	}
	else {
		VIPS_DEBUG_MSG( "tile_copy: zero filling for %dx%d\n",
			tile->area.left, tile->area.top ); 
		im_region_paint( to, &ovlap, 0 );
	}
}

/* Loop over the output region, filling with data from cache.
 */
static int
region_fill( REGION *out, void *seq, void *a, void *b )
{
	Render *render = (Render *) a;
	Rect *r = &out->valid;
	int x, y;

	/* Find top left of tiles we need.
	 */
	int xs = (r->left / render->tile_width) * render->tile_width;
	int ys = (r->top / render->tile_height) * render->tile_height;

	VIPS_DEBUG_MSG( "region_fill: left = %d, top = %d, "
		"width = %d, height = %d\n",
                r->left, r->top, r->width, r->height );

	g_mutex_lock( render->lock );

	/* 

		FIXME ... if r fits inside a single tile, we could skip the 
		copy.

	 */

	for( y = ys; y < IM_RECT_BOTTOM( r ); y += render->tile_height )
		for( x = xs; x < IM_RECT_RIGHT( r ); x += render->tile_width ) {
			Rect area;
			Tile *tile;

			area.left = x;
			area.top = y;
			area.width = render->tile_width;
			area.height = render->tile_height;

			tile = tile_request( render, &area );
			if( tile )
				tile_copy( tile, out );
		}

	g_mutex_unlock( render->lock );

	return( 0 );
}

/* The mask image is 255 / 0 for the state of painted for each tile.
 */
static int
mask_fill( REGION *out, void *seq, void *a, void *b )
{
	Render *render = (Render *) a;
	Rect *r = &out->valid;
	int x, y;

	/* Find top left of tiles we need.
	 */
	int xs = (r->left / render->tile_width) * render->tile_width;
	int ys = (r->top / render->tile_height) * render->tile_height;

	VIPS_DEBUG_MSG( "mask_fill: left = %d, top = %d, "
		"width = %d, height = %d\n",
                r->left, r->top, r->width, r->height );

	g_mutex_lock( render->lock );

	for( y = ys; y < IM_RECT_BOTTOM( r ); y += render->tile_height )
		for( x = xs; x < IM_RECT_RIGHT( r ); x += render->tile_width ) {
			Rect area;
			Tile *tile;

			area.left = x;
			area.top = y;
			area.width = render->tile_width;
			area.height = render->tile_height;

			tile = render_tile_lookup( render, &area );

			/* Only mark painted tiles containing valid pixels.
			 */
			im_region_paint( out, &area, 
				(tile && 
				tile->painted &&
				!tile->region->invalid) ? 255 : 0 );
		}

	g_mutex_unlock( render->lock );

	return( 0 );
}

/**
 * vips_sink_screen:
 * @in: input image
 * @out: output image
 * @mask: mask image indicating valid pixels
 * @tile_width: tile width
 * @tile_height: tile height
 * @max_tiles: maximum tiles to cache
 * @priority: rendering priority
 * @notify: pixels are ready notification callback
 * @a: client data for callback
 *
 * This operation renders @in in the background, making pixels available on
 * @out as they are calculated. The @notify callback is run every time a new
 * set of pixels are available. Calculated pixels are kept in a cache with
 * tiles sized @tile_width by @tile_height pixels and at most @max_tiles tiles.
 * If @max_tiles is -1, the cache is of unlimited size (up to the maximum image
 * size).
 * The @mask image s a one-band uchar image and has 255 for pixels which are 
 * currently in cache and 0 for uncalculated pixels.
 *
 * The pixel rendering system has a single global #im_threadgroup_t which is 
 * used for all currently active instances of im_render_priority(). As
 * renderers are added and removed from the system, the threadgroup switches
 * between renderers based on their priority setting. Zero means normal
 * priority, negative numbers are low priority, positive numbers high
 * priority.
 *
 * Calls to im_prepare() on @out return immediately and hold whatever is
 * currently in cache for that #Rect (check @mask to see which parts of the
 * #Rect are valid). Any pixels in the #Rect which are not in cache are added
 * to a queue, and the @notify callback will trigger when those pixels are
 * ready.
 *
 * The @notify callback is run from the background thread. In the callback,
 * you need to somehow send a message to the main thread that the pixels are
 * ready. In a glib-based application, this is easily done with g_idle_add().
 *
 * If @notify is %NULL, then im_render_priority() runs synchronously.
 * im_prepare() on @out will always block until the pixels have been
 * calculated by the background #im_threadgroup_t.
 *
 * See also: im_cache(), im_prepare().
 *
 * Returns: 0 on sucess, -1 on error.
 */
int
vips_sink_screen( VipsImage *in, VipsImage *out, VipsImage *mask, 
	int tile_width, int tile_height, 
	int max_tiles, 
	int priority,
	VipsSinkNotify notify, void *a )
{
	Render *render;

	/* Make sure the bg work threads are ready.
	 */
	if( render_thread_create() )
		return( -1 );

	if( tile_width <= 0 || tile_height <= 0 || 
		max_tiles < -1 ) {
		im_error( "vips_sink_screen", "%s", _( "bad parameters" ) );
		return( -1 );
	}

	if( im_piocheck( in, out ) ||
		im_cp_desc( out, in ) ||
		im_demand_hint( out, IM_SMALLTILE, in, NULL ) )
		return( -1 );
	if( mask ) {
		if( im_poutcheck( mask ) ||
			im_cp_desc( mask, in ) ||
			im_demand_hint( mask, IM_SMALLTILE, in, NULL ) )
			return( -1 );

		mask->Bands = 1;
		mask->BandFmt = IM_BANDFMT_UCHAR;
		mask->Type = IM_TYPE_B_W;
		mask->Coding = IM_CODING_NONE;
	}

	if( !(render = render_new( in, out, mask, 
		tile_width, tile_height, max_tiles, priority, notify, a )) )
		return( -1 );

	VIPS_DEBUG_MSG( "vips_sink_screen: max = %d, %p\n", max_tiles, render );

	if( im_generate( out, NULL, region_fill, NULL, render, NULL ) )
		return( -1 );
	if( mask && 
		im_generate( mask, NULL, mask_fill, NULL, render, NULL ) )
		return( -1 );

	return( 0 );
}

