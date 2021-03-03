


#ifndef __LOGIN_H
#define __LOGIN_H

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include "ui.h"

typedef struct loginmsg{
    GtkWidget* widget;
    GtkWidget* fwidget;
    GtkWidget* window;
    GtkWidget* username;
    GtkWidget* password;
}login_msg;
void login_on_button_clicked(GtkWidget* button,login_msg* data);
void login_to_regist_on_button_clicked(GtkWidget* button,gpointer data);
gboolean window_drag(GtkWidget *widget, GdkEventButton *event, GdkWindowEdge edge);
GtkWidget* loginUI();
#endif
