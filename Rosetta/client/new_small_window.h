


#ifndef __NEW_SMALL_WINDOW_H
#define __NEW_SMALL_WINDOW_H

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

void new_small_window_callback(GtkWidget* button,gpointer data);

GtkWidget* new_small_window();

void create_new_pop_window(const char* text);

#endif
