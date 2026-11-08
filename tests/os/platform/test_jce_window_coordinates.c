/* Actual window owner with counted SDL providers, never an OS/GPU window. */
#define SDL_CreateWindow fixture_create_window
#define SDL_DestroyWindow fixture_destroy_window
#define SDL_GetWindowSizeInPixels fixture_window_pixels
#define SDL_GetWindowSize fixture_window_points
#define SDL_Metal_CreateView fixture_metal_create
#define SDL_Metal_DestroyView fixture_metal_destroy
#define SDL_Metal_GetLayer fixture_metal_layer
#ifndef JCE_WINDOW_COORDINATE_OWNER
#define JCE_WINDOW_COORDINATE_OWNER "os/platform/jce_window.c"
#endif
#include JCE_WINDOW_COORDINATE_OWNER
#undef SDL_CreateWindow
#undef SDL_DestroyWindow
#undef SDL_GetWindowSizeInPixels
#undef SDL_GetWindowSize
#undef SDL_Metal_CreateView
#undef SDL_Metal_DestroyView
#undef SDL_Metal_GetLayer
#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_event.h>
#include "unity.h"
static unsigned point_queries,pixel_queries;
static int point_width,point_height,pixel_width,pixel_height;
static unsigned char native_tag,metal_tag;
static JceWindow *active_window;
static JceInput *active_input;
static SDL_Window *native(void) {return (SDL_Window *)&native_tag;}
SDL_Window *SDLCALL fixture_create_window(const char *title,int w,int h,SDL_WindowFlags flags)
{(void)title;(void)flags;TEST_ASSERT_EQUAL_INT(320,w);TEST_ASSERT_EQUAL_INT(200,h);return native();}
void SDLCALL fixture_destroy_window(SDL_Window *win) {TEST_ASSERT_EQUAL_PTR(native(),win);}
bool SDLCALL fixture_window_pixels(SDL_Window *win,int *w,int *h)
{TEST_ASSERT_EQUAL_PTR(native(),win);++pixel_queries;if(w)*w=pixel_width;if(h)*h=pixel_height;return true;}
bool SDLCALL fixture_window_points(SDL_Window *win,int *w,int *h)
{TEST_ASSERT_EQUAL_PTR(native(),win);++point_queries;if(w)*w=point_width;if(h)*h=point_height;return true;}
SDL_MetalView SDLCALL fixture_metal_create(SDL_Window *win)
{TEST_ASSERT_EQUAL_PTR(native(),win);return (SDL_MetalView)&metal_tag;}
void SDLCALL fixture_metal_destroy(SDL_MetalView view) {TEST_ASSERT_EQUAL_PTR(&metal_tag,view);}
void *SDLCALL fixture_metal_layer(SDL_MetalView view) {TEST_ASSERT_EQUAL_PTR(&metal_tag,view);return &metal_tag;}
void setUp(void)
{
    point_queries=pixel_queries=0;point_width=640;point_height=400;pixel_width=960;pixel_height=600;
    JceWindowConfig cfg={"point-coordinate-control",320,200,JCE_WINDOW_HIGH_PIXEL_DENSITY};
    active_window=jce_window_create(&cfg);TEST_ASSERT_NOT_NULL(active_window);
}
void tearDown(void)
{
    if(active_input){jce_input_destroy(active_input);active_input=NULL;}
    if(active_window){jce_window_destroy(active_window);active_window=NULL;}
}
static void actual_points_are_independent_of_configured_layout_and_queries_are_cached(void)
{
    uint32_t w=0,h=0;jce_window_get_point_size(active_window,&w,&h);
    TEST_ASSERT_EQUAL_UINT32(640,w);TEST_ASSERT_EQUAL_UINT32(400,h);
    float x=-1,y=-1;
    for(int i=0;i<8;++i){TEST_ASSERT_TRUE(jce_window_point_to_pixels(active_window,101,50,&x,&y));}
    TEST_ASSERT_EQUAL_FLOAT(151.5f,x);TEST_ASSERT_EQUAL_FLOAT(75,y);
    TEST_ASSERT_EQUAL_UINT(1,point_queries);TEST_ASSERT_EQUAL_UINT(1,pixel_queries);
    TEST_ASSERT_TRUE(jce_window_point_to_pixels(active_window,-10,401,&x,&y));
    TEST_ASSERT_EQUAL_FLOAT(-15,x);TEST_ASSERT_EQUAL_FLOAT(601.5f,y); /* no clamped validity */
}
static void existing_resize_refreshes_actual_point_cache_without_per_read_queries(void)
{
    point_width=400;point_height=300;
    jce_window_handle_resize(active_window,800,450);
    float x=0,y=0;TEST_ASSERT_TRUE(jce_window_point_to_pixels(active_window,200,150,&x,&y));
    TEST_ASSERT_EQUAL_FLOAT(400,x);TEST_ASSERT_EQUAL_FLOAT(225,y);
    uint32_t w=0,h=0;jce_window_get_point_size(active_window,&w,&h);
    TEST_ASSERT_EQUAL_UINT32(400,w);TEST_ASSERT_EQUAL_UINT32(300,h);
    TEST_ASSERT_EQUAL_UINT(2,point_queries);TEST_ASSERT_EQUAL_UINT(1,pixel_queries);
    point_width=point_height=0;jce_window_handle_resize(active_window,800,450);
    TEST_ASSERT_FALSE(jce_window_point_to_pixels(active_window,200,150,&x,&y));
    TEST_ASSERT_EQUAL_FLOAT(400,x);TEST_ASSERT_EQUAL_FLOAT(225,y);
}
static void unavailable_nonfinite_and_null_inputs_preserve_outputs(void)
{
    float x=7,y=8;JceWindow empty={0};
    TEST_ASSERT_FALSE(jce_window_point_to_pixels(NULL,1,2,&x,&y));
    TEST_ASSERT_FALSE(jce_window_point_to_pixels(&empty,1,2,&x,&y));
    TEST_ASSERT_FALSE(jce_window_point_to_pixels(active_window,NAN,2,&x,&y));
    TEST_ASSERT_FALSE(jce_window_point_to_pixels(active_window,1,INFINITY,&x,&y));
    TEST_ASSERT_FALSE(jce_window_point_to_pixels(active_window,1,2,NULL,&y));
    TEST_ASSERT_FALSE(jce_window_point_to_pixels(active_window,1,2,&x,NULL));
    TEST_ASSERT_EQUAL_FLOAT(7,x);TEST_ASSERT_EQUAL_FLOAT(8,y);
    uint32_t w=99,h=99;jce_window_get_point_size(NULL,&w,&h);
    TEST_ASSERT_EQUAL_UINT32(0,w);TEST_ASSERT_EQUAL_UINT32(0,h);
}
static void mouse_and_normalized_touch_reach_same_canvas_pixels_once(void)
{
    active_input=jce_input_create();TEST_ASSERT_NOT_NULL(active_input);
    JceInputEvent event={0};event.size=sizeof event;event.kind=JCE_INPUT_EVENT_MOUSE_MOTION;
    event.motion.x=320;event.motion.y=100;event.motion.dx=8;event.motion.dy=4;
    jce_input_submit(active_input,&event,1);
    float dx=0,dy=0;jce_input_mouse_delta(active_input,&dx,&dy);
    TEST_ASSERT_TRUE(jce_window_point_to_pixels(active_window,dx,dy,&dx,&dy));
    TEST_ASSERT_EQUAL_FLOAT(12,dx);TEST_ASSERT_EQUAL_FLOAT(6,dy);
    uint32_t w=0,h=0;jce_window_get_point_size(active_window,&w,&h);
    float x=0,y=0;bool down=false;
    TEST_ASSERT_TRUE(jce_input_primary_pointer(active_input,(float)w,(float)h,&x,&y,&down));
    TEST_ASSERT_TRUE(jce_window_point_to_pixels(active_window,x,y,&x,&y));
    TEST_ASSERT_EQUAL_FLOAT(480,x);TEST_ASSERT_EQUAL_FLOAT(150,y);
    memset(&event,0,sizeof event);event.size=sizeof event;event.kind=JCE_INPUT_EVENT_TOUCH;
    event.touch.finger=1;event.touch.phase=JCE_INPUT_TOUCH_PHASE_DOWN;event.touch.x=0.5f;event.touch.y=0.25f;
    jce_input_submit(active_input,&event,1);
    TEST_ASSERT_TRUE(jce_input_primary_pointer(active_input,(float)w,(float)h,&x,&y,&down));
    TEST_ASSERT_TRUE(jce_window_point_to_pixels(active_window,x,y,&x,&y));
    TEST_ASSERT_TRUE(down);TEST_ASSERT_EQUAL_FLOAT(480,x);TEST_ASSERT_EQUAL_FLOAT(150,y);
    TEST_ASSERT_EQUAL_UINT(1,point_queries);TEST_ASSERT_EQUAL_UINT(1,pixel_queries);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(actual_points_are_independent_of_configured_layout_and_queries_are_cached);
    RUN_TEST(existing_resize_refreshes_actual_point_cache_without_per_read_queries);
    RUN_TEST(unavailable_nonfinite_and_null_inputs_preserve_outputs);
    RUN_TEST(mouse_and_normalized_touch_reach_same_canvas_pixels_once);
    return UNITY_END();
}
