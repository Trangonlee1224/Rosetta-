

#include "new_small_window.h"

GtkWidget* pop_window;
GtkWidget* new_small_window_label;



void new_small_window_callback(GtkWidget* button,gpointer data){
	gtk_widget_hide_all(pop_window);
}



GtkWidget* new_small_window(){
    GtkWidget* window;
    //GtkWidget* label;
    GtkWidget* button;
    GtkWidget* box;
    window=gtk_window_new(GTK_WINDOW_POPUP);
//    g_signal_connect(G_OBJECT(window),"destroy",G_CALLBACK(gtk_main_quit),NULL);
    //gtk_window_set_title(GTK_WINDOW(window),title);
    gtk_window_set_position(GTK_WINDOW(window),GTK_WIN_POS_CENTER);
    gtk_container_set_border_width(GTK_CONTAINER(window),20);


    new_small_window_label=gtk_label_new(NULL);
    box=gtk_vbox_new(FALSE,0);
    button=gtk_button_new_with_label("ok");
    g_signal_connect_swapped(G_OBJECT(button),"clicked",G_CALLBACK(new_small_window_callback),NULL);
    gtk_box_pack_start(GTK_BOX(box),new_small_window_label,FALSE,FALSE,5);
    gtk_box_pack_start(GTK_BOX(box),button,FALSE,FALSE,5);
    gtk_container_add(GTK_CONTAINER(window),box);
    //gtk_widget_show_all(window);
    //gtk_main();
	return window;
}



void create_new_pop_window(const char* text){
	gtk_label_set(GTK_LABEL(new_small_window_label),text);
	gtk_widget_show_all(pop_window);
}
