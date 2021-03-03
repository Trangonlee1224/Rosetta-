

#include "main_program.h"

extern GtkWidget* login_window;
extern GtkWidget* mainUI_window;
extern GtkWidget* regist_window;
extern GtkWidget* pop_window;


int main(int argc,char * argv[]){
	char server_ip[15];
	FILE *fp = fopen("serverip.cfg","r");
	fscanf(fp,"%s",server_ip);
	fclose(fp);

	if(!init_client(server_ip)) {
		printf("init failed\n");
		exit(0);
	}
	if (!g_thread_supported())
		g_thread_init(NULL);
	gdk_threads_init();
	gtk_init(&argc, &argv);
	login_window=loginUI();
	regist_window=registUI();
	mainUI_window=mainUI();
	pop_window=new_small_window();
	gtk_widget_show_all(login_window);
	//g_thread_create((GThreadFunc)UI_thread, NULL, FALSE, NULL);  
	g_thread_create((GThreadFunc)recv_thread, NULL, FALSE, NULL);  
	g_thread_create((GThreadFunc)auto_update_thread, NULL, FALSE, NULL);
	g_thread_create((GThreadFunc)rev_FILE_thread, NULL, FALSE, NULL);  
	//gdk_threads_enter(); 
	gtk_main(); 
	//gdk_threads_leave(); 

	return 0;
}
