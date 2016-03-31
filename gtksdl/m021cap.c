/*
   m021cap :  Capture and display YUYV images from LI-USB30-M021 using V4L2

   Copyright (C) 2016 Simon D. Levy

   Adapted from GUVCView: http://guvcview.sourceforge.net

   This file is part of M021_V4L2.

   M021_V4L2 is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   M021_V4L2 is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   You should have received a copy of the GNU General Public License
   along with M021_V4L2.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <gtk/gtk.h>
#include <SDL/SDL.h>
#include <glib/gi18n.h>
#include <glib/gprintf.h>
#include <fcntl.h>		

#include "../m021_v4l2.h"

// Other possibilities are 800x460 and 640x480 ----------------

#define VDIN_T vdIn_1280x720_t
#define WIDTH 1280
#define HEIGHT 720
#define VD_INIT m021_1280x720_init
#define VD_GRAB m021_1280x720_grab_yuyv

// -------------------------------------------------------------

#define __THREAD_JOIN(t) (pthread_join(t, NULL))

#define __MUTEX_TYPE pthread_mutex_t
#define __INIT_MUTEX(m) ( pthread_mutex_init(m, NULL) )

static	pthread_mutex_t      mutex;      
static int                   hwaccel;    
static int                   bpp;        
static char *                caption;    
static gboolean              signalquit;
static	int                  framewidth; 
static	int                  frameheight;
static VDIN_T *              videoIn;
static pthread_t             video_thread;
static const SDL_VideoInfo * info;
static	gboolean             vid_widget_state;

static Uint32 SDL_VIDEO_Flags = SDL_ANYFORMAT | SDL_RESIZABLE; 

static void shutdown (void)
{
    signalquit = TRUE;
    pthread_join(video_thread, NULL);
    gtk_main_quit();

}
static int shutdown_timer(gpointer data)
{
    shutdown ();
    return FALSE;
}

static SDL_Overlay * video_init(SDL_Surface **pscreen)
{
    int width = framewidth;
    int height = frameheight;

    if (*pscreen == NULL) 
    {

        if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER) < 0)
        {
            g_printerr("Couldn't initialize SDL: %s\n", SDL_GetError());
            exit(1);
        }


        if(hwaccel)
        {
            if ( ! getenv("SDL_VIDEO_YUV_HWACCEL") ) putenv("SDL_VIDEO_YUV_HWACCEL=1");
            if ( ! getenv("SDL_VIDEO_YUV_DIRECT") ) putenv("SDL_VIDEO_YUV_DIRECT=1");
        }
        else
        {
            if ( ! getenv("SDL_VIDEO_YUV_HWACCEL") ) putenv("SDL_VIDEO_YUV_HWACCEL=0");
            if ( ! getenv("SDL_VIDEO_YUV_DIRECT") ) putenv("SDL_VIDEO_YUV_DIRECT=0");
        }

        info = SDL_GetVideoInfo();

        if (info->hw_available)
        {
            SDL_VIDEO_Flags |= SDL_HWSURFACE;
            SDL_VIDEO_Flags |= SDL_DOUBLEBUF;
        }
        else
        {
            SDL_VIDEO_Flags |= SDL_SWSURFACE;
        }

        if (info->blit_hw)
        {
            SDL_VIDEO_Flags |= SDL_ASYNCBLIT;
        }

        SDL_WM_SetCaption(caption, NULL);

        
        SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY,SDL_DEFAULT_REPEAT_INTERVAL);
    }

    g_print("Checking video mode %ix%i@32bpp : ", width, height);
    int bpp = SDL_VideoModeOK( width, height, 32, SDL_VIDEO_Flags);

    *pscreen = SDL_SetVideoMode(
        width,
        height,
        bpp,
        SDL_VIDEO_Flags);

    if(*pscreen == NULL)
    {
        return NULL;
    }
    
    SDL_Overlay* overlay=NULL;
    overlay = SDL_CreateYUVOverlay(framewidth, frameheight, SDL_YUY2_OVERLAY, *pscreen);

    SDL_ShowCursor(SDL_DISABLE);
    return overlay;
}

static void *main_loop()
{
    SDL_Event event;
    SDL_Surface *pscreen = NULL;
    SDL_Overlay *overlay = NULL;
    SDL_Rect drect;

    uint8_t *p = NULL;

    signalquit = FALSE;

    overlay = video_init(&(pscreen));

    if(overlay == NULL)
    {
        g_print("FATAL: Couldn't create yuv overlay - please disable hardware accelaration\n");
        signalquit = TRUE; // exit video thread
    }
    else
    {
        p = (unsigned char *) overlay->pixels[0];

        drect.x = 0;
        drect.y = 0;
        drect.w = pscreen->w;
        drect.h = pscreen->h;
    }

    while (!signalquit)
    {
        SDL_LockYUVOverlay(overlay);
        if (VD_GRAB(videoIn, p) < 0) {
            g_printerr("Error grabbing image \n");
            continue;
        }
        SDL_UnlockYUVOverlay(overlay);
        SDL_DisplayYUVOverlay(overlay, &drect);

        while( SDL_PollEvent(&event) )
        {
            if(event.type==SDL_VIDEORESIZE)
            {
                pscreen = SDL_SetVideoMode(event.resize.w, event.resize.h, bpp, SDL_VIDEO_Flags);
                drect.w = event.resize.w;
                drect.h = event.resize.h;
            }
            if(event.type==SDL_QUIT)
            {
                g_timeout_add(200, shutdown_timer, NULL);
            }
        }

    } // loop end


    p = NULL;

    if(overlay)
        SDL_FreeYUVOverlay(overlay);

    SDL_Quit();

    videoIn = NULL;

    return (void *) 0;
}

/*
   Unix signals that are cought are written to a pipe. The pipe connects
   the unix signal handler with GTK's event loop. The array signal_pipe will
   hold the file descriptors for the two ends of the pipe (index 0 for
   reading, 1 for writing).
 */
static int signal_pipe[2];

/*
   The unix signal handler.
   Write any unix signal into the pipe. The writing end of the pipe is in
   non-blocking mode. If it is full (which can only happen when the
   event loop stops working) signals will be dropped.
 */
static void pipe_signals(int signal)
{
  if(write(signal_pipe[1], &signal, sizeof(int)) != sizeof(int))
    {
      fprintf(stderr, "unix signal %d lost\n", signal);
    }
}

/*
   The event loop callback that handles the unix signals. Must be a GIOFunc.
   The source is the reading end of our pipe, cond is one of
     G_IO_IN or G_IO_PRI (I don't know what could lead to G_IO_PRI)
   the pointer d is always NULL
 */
static gboolean deliver_signal(GIOChannel *source, GIOCondition cond, gpointer data)
{
  GError *error = NULL;		/* for error handling */

  /*
     There is no g_io_channel_read or g_io_channel_read_int, so we read
     char's and use a union to recover the unix signal number.
   */
  union {
    gchar chars[sizeof(int)];
    int signal;
  } buf;
  GIOStatus status;		// save the reading status 
  gsize bytes_read;		//* save the number of chars read 

  /*
     Read from the pipe as long as data is available. The reading end is
     also in non-blocking mode, so if we have consumed all unix signals,
     the read returns G_IO_STATUS_AGAIN.
   */
  while((status = g_io_channel_read_chars(source, buf.chars,
		     sizeof(int), &bytes_read, &error)) == G_IO_STATUS_NORMAL)
    {
      g_assert(error == NULL);	/* no error if reading returns normal */

      // There might be some problem resulting in too few char's read.  Check it.
      if(bytes_read != sizeof(int)){
	fprintf(stderr, "lost data in signal pipe (expected %lu, received %lu)\n",
		(long unsigned int) sizeof(int), (long unsigned int) bytes_read);
	continue;	      /* discard the garbage and keep fingers crossed */
      }

      // Ok, we read a unix signal number, so let the label reflect it!
     switch (buf.signal)
     {
     	case SIGINT:
     		shutdown();
     		break;
    	default:
    		printf("guvcview signal %d caught\n", buf.signal);
    		break;
     }
    }

  /*
     Reading from the pipe has not returned with normal status. Check for
     potential errors and return from the callback.
   */
  if(error != NULL){
    fprintf(stderr, "reading signal pipe failed: %s\n", error->message);
    exit(1);
  }
  if(status == G_IO_STATUS_EOF){
    fprintf(stderr, "signal pipe has been closed\n");
    exit(1);
  }

  g_assert(status == G_IO_STATUS_AGAIN);
  return TRUE;		/* keep the event source */
}

int main(int argc, char *argv[])
{
	/*
   	  In order to register the reading end of the pipe with the event loop
   	  we must convert it into a GIOChannel.
   	*/
  	GIOChannel *g_signal_in;
  	long fd_flags; 	    /* used to change the pipe into non-blocking mode */
  	GError *error = NULL;	/* handle errors */

	__INIT_MUTEX(&mutex);

	caption = g_new(char, 32);

	g_sprintf(caption,"LI-USB30-M021");

	bpp = 0; //current bytes per pixel
	hwaccel = 1; //use hardware acceleration
	framewidth = WIDTH;
	frameheight = HEIGHT;

	vid_widget_state = TRUE;

    if(!gtk_init_check(&argc, &argv))
    {
        g_printerr("can't open display\n");
        exit(1);
    }

    g_set_application_name(_("LEOPARD Video Capture"));

    // make sure the type is realized so that we can change the properties
    g_type_class_unref (g_type_class_ref (GTK_TYPE_BUTTON));

    // make sure gtk-button-images property is set to true (defaults to false in karmic)
    g_object_set (gtk_settings_get_default (), "gtk-button-images", TRUE, NULL);

    videoIn = g_new0(VDIN_T, 1);

    framewidth =  WIDTH;
    frameheight = HEIGHT;

    VD_INIT("/dev/video0", videoIn);

    if(pthread_create(&video_thread, NULL, main_loop, NULL))
    {
        g_printerr("Video thread creation failed\n");
    }

    if(pipe(signal_pipe))
    {
        perror("pipe");
        exit(1);
    }

    /*
       put the write end of the pipe into nonblocking mode,
       need to read the flags first, otherwise we would clear other flags too.
     */
    fd_flags = fcntl(signal_pipe[1], F_GETFL);
    if(fd_flags == -1)
    {
        perror("read descriptor flags");
    }
    if(fcntl(signal_pipe[1], F_SETFL, fd_flags | O_NONBLOCK) == -1)
    {
        perror("write descriptor flags");
    }

    // Install the unix signal handler pipe_signals for the signals of interest 
    signal(SIGINT, pipe_signals);

    //  convert the reading end of the pipe into a GIOChannel
    g_signal_in = g_io_channel_unix_new(signal_pipe[0]);

    // we only read raw binary data from the pipe, therefore clear any encoding on the channel
    g_io_channel_set_encoding(g_signal_in, NULL, &error);
    if(error != NULL)
    {
        // handle potential errors 
        fprintf(stderr, "g_io_channel_set_encoding failed %s\n",
                error->message);
    }

    // put the reading end also into non-blocking mode 
    g_io_channel_set_flags(g_signal_in,
            g_io_channel_get_flags(g_signal_in) | G_IO_FLAG_NONBLOCK, &error);

    if(error != NULL)
        fprintf(stderr, "g_io_set_flags failed %s\n", error->message);

    // register the reading end with the event loop 
    g_io_add_watch(g_signal_in, G_IO_IN | G_IO_PRI, deliver_signal, NULL);


    gtk_main();
    
    return 0;
}
