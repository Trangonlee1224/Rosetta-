


#include "server.h"


int listenfd, connectfd;
struct sockaddr_in client, server;

const char * welcome_message = "welcome";

char buffer[BUFFER_SIZE];

struct send_prop send_queue[MAX_SEND_QUEUE_SIZE];
int send_q_head, send_q_tail;
//lock for the queue
static pthread_mutex_t queue_lock;
struct client_property client_prop[MAX_CLIENT_COUNT];

//sqlite3 database
sqlite3 *db;

//log file
FILE* logfile;
char log_buffer[4096];

//server running mode
int LOG_ON = 1;
int DEBUG = 1;
int PRESSURE_TEST = 0;



char * copy_string(const char *src) {
	char *res = (char *)malloc(sizeof(char) * (strlen(src) + 1));
	strcpy(res, src);
	return res;
}



struct client_property * get_user_prop(char *username) {
	if(username == NULL) return NULL;
	int i;
	for(i = 0; i < MAX_CLIENT_COUNT; i++) {
		if(strcmp(username, client_prop[i].username) == 0) {
			return &(client_prop[i]);
		}
	}
	return NULL;
}



char *sock_ntop(const struct sockaddr *sa)
{
	static char str[128];
      struct sockaddr_in *sin = (struct sockaddr_in *)sa;
      if (inet_ntop(AF_INET, &sin->sin_addr, str, sizeof(str))==NULL)
        return NULL;
      return str;
}


char *get_user_ip(char *username){
	if(username==NULL) return -1;
	int i;
	char *str;
	for( i = 0;i< MAX_CLIENT_COUNT;i++){
		if(strcmp(username,client_prop[i].username) == 0){
			return sock_ntop(&client_prop[i].addr);		
		}			
	}
}


int db_init() {
	if(SQLITE_OK != sqlite3_open("server.db", &db))
		return 0;
	sqlite3_exec(db, "create table if not exists alluser(username TEXT PRIMARY KEY, password TEXT NOT NULL)",
		NULL, NULL, NULL);
	sqlite3_exec(db, "create table if not exists contacts(username TEXT, contact TEXT)",
		NULL, NULL, NULL);
	sqlite3_exec(db, "create table if not exists grouplist(groupID INTEGER PRIMARY KEY autoincrement NOT NULL, creator TEXT)",
		NULL, NULL, NULL);
	sqlite3_exec(db, "create table if not exists groupmember(groupID INTEGER NOT NULL, member TEXT NOT NULL)",
		NULL, NULL, NULL);
	sqlite3_exec(db, "create table if not exists offlinemsg(sendto TEXT NOT NULL, msg TEXT NOT NULL)",
		NULL, NULL, NULL);
	sqlite3_exec(db, "create table if not exists userimage(username TEXT PRIMARY KEY, image TEXT NOT NULL)",
		NULL, NULL, NULL);
	return 1;
}



//===================SERVER LOGIC=================================
void user_register(struct client_property *prop, char *username, char *password) {
	printf("register %s %s\n", username, password);
	char sqlcmd[2048];
	memset(sqlcmd, '\0', sizeof(sqlcmd));
	sprintf(sqlcmd, "insert into alluser values(\'%s\', \'%s\')", username, password);
	int ret = sqlite3_exec(db, sqlcmd, NULL, NULL, NULL);
	if(ret == SQLITE_OK) {
		sprintf(sqlcmd, "{\"type\":\"register-receipt\", \"username\":\"%s\", \"status\":1}", username);
	} else {
		sprintf(sqlcmd, "{\"type\":\"register-receipt\", \"username\":\"%s\", \"status\":0}", username);
	}

	send_message_by_fd(prop->client_fd, sqlcmd);
	memset(sqlcmd, '\0', sizeof(sqlcmd));
	sprintf(sqlcmd, "insert into userimage values(\'%s\', \'0\')", username);
	ret = sqlite3_exec(db, sqlcmd, NULL, NULL, NULL);
}



void user_login(struct client_property *prop, char *username, char *password) {
	//TODO: username and password might need to free
	//TODO: one user may not login twice, and one client may not login two users!
	printf("login %s %s\n", username, password);
	char sqlcmd[2048];
	memset(sqlcmd, '\0', sizeof(sqlcmd));
	sprintf(sqlcmd, "select password from alluser where username=\'%s\'", username);
	char **res; char *errmsg; int nrow; int ncol;
	if(SQLITE_OK != sqlite3_get_table(db, sqlcmd, &res, &nrow, &ncol, &errmsg)) {
		printf("error while login: %s\n", errmsg);
		return ;
	}
	char jsonstr[1024];

	int status;
	if(nrow == 0) {
		printf("no such user\n");
		status = -1;
	} else if(strcmp(res[1], password) == 0) {
		printf("password verified\n");

		struct client_property * prop1 = get_user_prop(username);
		if(prop1 != NULL) {
			sprintf(jsonstr, "{\"type\":\"force-logout-notif\",\"username\":\"%s\"}", username);
			send_message_by_fd(prop1->client_fd, jsonstr);
			sprintf(jsonstr, "{\"type\":\"login-receipt\",\"status\":%d,\"username\":\"%s\"}", 0, username);
			send_message_by_fd(prop->client_fd, jsonstr);
			user_logout(prop1, username);
			return ;
		}

		prop->is_online = 1;
		strcpy(prop->username, username);
		send_offline_message(username);
		status = 1;
	} else {
		printf("password error");
		status = 0;
	}
	int user_image=0;
	if(status == 1){
		memset(sqlcmd, '\0', sizeof(sqlcmd));
		sprintf(sqlcmd, "select image from userimage where username=\'%s\'", username);
		if(SQLITE_OK != sqlite3_get_table(db, sqlcmd, &res, &nrow, &ncol, &errmsg)) {
			printf("error while login(select userimage): %s\n", errmsg);
			return ;
		}
		printf("select image-------- res[1]:%s\n",res[1]);
		user_image = res[1][0] - '0';
	}
	sprintf(sqlcmd, "{\"type\":\"login-receipt\",\"status\":%d,\"username\":\"%s\",\"image\":%d}", status, username, user_image);
	send_message_by_fd(prop->client_fd, sqlcmd);
}


void user_logout(struct client_property *prop, char *username) {
	if(strcmp(prop->username, username) != 0) {
		return ;
	}
	printf("user %s logout\n", username);
	memset(prop->username, '\0', sizeof(prop->username));
	prop->is_online = 0;
}



int send_single_message_callback(void *arg, int nr, char **values, char **names){
	if(DEBUG) printf("send_single_message_callback %s\n", values[0]);
	send_message_by_username(values[0], copy_string((char *)arg));
   	return 0;
}



//char *message should be static...
void send_single_message(char *message) {
	cJSON *root = cJSON_Parse(message);
	char *sendfrom = cJSON_GetObjectItem(root, "sendfrom")->valuestring;
	char *sendto = cJSON_GetObjectItem(root, "sendto")->valuestring;
	char *content = cJSON_GetObjectItem(root, "content")->valuestring;
	char sqlcmd[2048];
	memset(sqlcmd, '\0', sizeof(sqlcmd));
	sprintf(sqlcmd, "select username from alluser where username=\'%s\'", sendto);
	sqlite3_exec(db, sqlcmd, send_single_message_callback, message, NULL);
}


void send_return_message(char *message){
	cJSON *root = cJSON_Parse(message);
	char *sendfrom = cJSON_GetObjectItem(root, "sendfrom")->valuestring;
	char *sendto = cJSON_GetObjectItem(root, "sendto")->valuestring;
	char *content = cJSON_GetObjectItem(root, "content")->valuestring;
	char sqlcmd[2048];
	char *ipfd = get_user_ip(sendto);
	cJSON *Root = cJSON_CreateObject();
	if(ipfd == NULL){
        printf("create json object failed\n");
        return;
	}
	printf("yifasonghuiqu");
	cJSON_AddStringToObject(Root, "type","return-message-filetransform");
	cJSON_AddStringToObject(Root, "Ipfd", ipfd);
	cJSON_AddStringToObject(Root, "sendto", sendfrom);
	cJSON_AddStringToObject(Root, "sendfrom", sendfrom);
	cJSON_AddStringToObject(Root, "content", content);
	send_message_by_username(sendfrom,copy_string(cJSON_Print(Root)));
}



void send_friend_list(char *username) {
	char sqlcmd[1024];
	memset(sqlcmd, '\0', sizeof(sqlcmd));
	sprintf(sqlcmd, "select contact from contacts where username=\'%s\'", username);
	char **res; char *errmsg; int nrow; int ncol;
	if(SQLITE_OK != sqlite3_get_table(db, sqlcmd, &res, &nrow, &ncol, &errmsg)) {
		printf("error while reading friend list: %s\n", errmsg);
		return ;
	}
	cJSON *root = cJSON_CreateObject();
	cJSON *list = cJSON_CreateArray();

	char contact_name[32];
	int i, j; int nindex = ncol;
	for(i=0;i<nrow;i++){
		for(j=0;j<ncol;j++){
			//printf("%s ", res[nindex]);
			strcpy(contact_name, res[nindex]);
			int status = (get_user_fd(contact_name) != -1);
			cJSON *item;
			cJSON_AddItemToArray(list, item = cJSON_CreateObject());
			cJSON_AddStringToObject(item, "username", copy_string(contact_name));
			cJSON_AddNumberToObject(item, "status", status);
			nindex++;
		}
		//printf("\n");
	}
	cJSON_AddStringToObject(root, "type", "friend-list");
	cJSON_AddNumberToObject(root, "size", nrow);
	cJSON_AddItemToObject(root, "list", list);
	char *jsonstr = copy_string(cJSON_Print(root));
	send_message_by_username(username, jsonstr);
}



void add_contact(char *ua, char *ub){
	printf("%s trying to add %s as contact\n", ua, ub);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "add-to-contact-receipt");
    cJSON_AddStringToObject(root, "username", ua);
    cJSON_AddStringToObject(root, "contact", ub);
	char sqlcmd[BUFFER_SIZE];
	char ** res; char * errmsg; int nrow = 0, ncol = 0;
	
    if(strcmp(ua, ub) == 0){
        cJSON_AddNumberToObject(root, "status", -1);
    } else {
		memset(sqlcmd, '\0', sizeof(sqlcmd));		
		sprintf(sqlcmd, "select username from alluser where username=\'%s\'", ub);
		sqlite3_get_table(db, sqlcmd, &res, &nrow, &ncol, &errmsg);
		//user does not exist
		printf("%d\n", nrow);
		if(nrow != 1) {
			cJSON_AddNumberToObject(root, "status", 0);
		} else {
		    memset(sqlcmd, '\0', sizeof(sqlcmd));
			//already contact?
		    sprintf(sqlcmd, "select * from contacts where username=\'%s\' and contact=\'%s\'", ua, ub);
		    sqlite3_get_table(db, sqlcmd, &res, &nrow, &ncol, &errmsg);
		    if(nrow == 1) {
		        cJSON_AddNumberToObject(root, "status", 0);
		        return ;
		    } else {
		        sprintf(sqlcmd, "insert into contacts values(\'%s\',\'%s\')", ua, ub);
		        printf("%s\n", sqlcmd);
		        if(sqlite3_exec(db, sqlcmd, NULL, NULL, NULL) == SQLITE_OK) {
		            cJSON_AddNumberToObject(root, "status", 1);
		        } else {
		            cJSON_AddNumberToObject(root, "status", 0);
		        }
		    }
		}
	}
	send_message_by_username(ua, copy_string(cJSON_Print(root)));
}


void create_group_callback(void *arg, int nr, char **values, char **names) {
	char *username = (char *)arg;
	printf("create_group_callback %s %s\n", values[0], username);
	char *message = (char*)malloc(sizeof(char)*BUFFER_SIZE);
	sprintf(message, "{\
	\"type\":\"group-create-receipt\",\
	\"status\":1,\
	\"groupID\":%s}", values[0]);
	send_message_by_username(username, message);
	int groupID;
	sscanf(values[0], "%d", &groupID);
	join_group(username, groupID);
}


void create_group(char *username) {
	char sqlcmd[1024];
	printf("creating group for %s\n", username);
	sprintf(sqlcmd, "insert into grouplist values(null, \'%s\')", username);
	int ret = sqlite3_exec(db, sqlcmd, NULL, NULL, NULL);
	if(ret == SQLITE_OK) {
		sprintf(sqlcmd, "select groupID from grouplist where creator=\'%s\' order by groupID desc limit 0, 1", username);
		ret = sqlite3_exec(db, sqlcmd, create_group_callback, username, NULL);
	} else {
		char message = (char *)malloc(sizeof(char)*BUFFER_SIZE);
		sprintf(message, "{\
		\"type\":\"group-create-receipt\",\
		\"status\":0}");
		send_message_by_username(username, message);
	}
}


int group_exist(int groupID){
    char **res; char *errmsg; int nrow, ncol;
	char sqlmsg[1024];
    sprintf(sqlmsg, "select * from grouplist where groupID=%d", groupID);
	int ret = sqlite3_get_table(db, sqlmsg, &res, &nrow, &ncol, &errmsg);
	return (ret == SQLITE_OK && nrow == 1);
}



void join_group(char *username, int groupID) {
	printf("user %s joining group %d\n", username, groupID);
	cJSON *root = cJSON_CreateObject();
	if(root == NULL) return ;
	cJSON_AddStringToObject(root, "type", "group-join-receipt");
	cJSON_AddStringToObject(root, "username", username);
	cJSON_AddNumberToObject(root, "groupID", groupID);
    if(!group_exist(groupID)) {
        printf("group %d not exist\n", groupID);
		cJSON_AddNumberToObject(root, "status", 0);
		send_message_by_username(username, copy_string(cJSON_Print(root)));
        return ;
    }
	char **res; char *errmsg; int nrow, ncol;
	char sqlmsg[1024];
	sprintf(sqlmsg, "select * from groupmember where groupID=%d and member=\'%s\'", groupID, username);
	int ret = sqlite3_get_table(db, sqlmsg, &res, &nrow, &ncol, &errmsg);
	if(ret != SQLITE_OK) return ;
	int status = 0;
	if(nrow == 0) {
		sprintf(sqlmsg, "insert into groupmember values(%d, \'%s\')", groupID, username);
		ret = sqlite3_exec(db, sqlmsg, NULL, NULL, NULL);
		if(ret == SQLITE_OK){
			printf("user %s join group %d success\n", username, groupID);
			status = 1;
		} else {
			printf("user %s join group %d fail\n", username, groupID);
			status = 0;
		}
	} else {
		printf("user %s already in group %d\n", username, groupID);
		status = 0;
	}
	cJSON_AddNumberToObject(root, "status", status);
	send_message_by_username(username, cJSON_Print(root));
}



void quit_group(char *username, int groupID) {
	cJSON *root = cJSON_CreateObject();
	if(root == NULL) return ;
	cJSON_AddStringToObject(root, "type", "group-quit-receipt");
	cJSON_AddStringToObject(root, "username", username);
	cJSON_AddNumberToObject(root, "groupID", groupID);
    if(!group_exist(groupID)) {
        printf("group %d not exist\n", groupID);
		cJSON_AddNumberToObject(root, "status", 0);
		send_message_by_username(username, cJSON_Print(root));
        return ;
    }
	char **res; char *errmsg; int nrow, ncol;
	char sqlmsg[1024];
    if(!group_exist(groupID)) {
        printf("group %d not exist\n", groupID);
        return ;
    }
	sprintf(sqlmsg, "select * from groupmember where groupID=%d and member=\'%s\'", groupID, username);
	int ret = sqlite3_get_table(db, sqlmsg, &res, &nrow, &ncol, &errmsg);
	if(ret != SQLITE_OK) return ;
	int status = 0;
	if(nrow == 1) {
		sprintf(sqlmsg, "delete from groupmember where groupID=%d and member=\'%s\'", groupID, username);
		ret = sqlite3_exec(db, sqlmsg, NULL, NULL, NULL);
		if(ret == SQLITE_OK){
			printf("user %s quit group %d success\n", username, groupID);
			status = 1;
		} else {
			printf("user %s quit group %d fail\n", username, groupID);
			status = 0;
		}
	} else {
		printf("user %s quit group %d fail\n", username, groupID);
		status = 0;
	}
	cJSON_AddNumberToObject(root, "status", status);
	send_message_by_username(username, cJSON_Print(root));
}


void send_group_list(char *username) {
	char sqlcmd[1024];
	memset(sqlcmd, '\0', sizeof(sqlcmd));
	sprintf(sqlcmd, "select groupID from groupmember where member=\'%s\'", username);
	char **res; char *errmsg; int nrow; int ncol;
	if(SQLITE_OK != sqlite3_get_table(db, sqlcmd, &res, &nrow, &ncol, &errmsg)) {
		printf("error while reading group list: %s\n", errmsg);
		return ;
	}
	cJSON *root = cJSON_CreateObject();
	cJSON *list = cJSON_CreateArray();

	int groupID;
	int i, j; int nindex = ncol;
	for(i=0;i<nrow;i++){
		for(j=0;j<ncol;j++){
			sscanf(res[nindex], "%d", &groupID);
			cJSON *item;
			cJSON_AddItemToArray(list, item = cJSON_CreateObject());
			cJSON_AddNumberToObject(item, "groupID", groupID);
			nindex++;
		}
		//printf("\n");
	}
	cJSON_AddStringToObject(root, "type", "group-list");
	cJSON_AddNumberToObject(root, "size", nrow);
	cJSON_AddItemToObject(root, "list", list);
	char *jsonstr = copy_string(cJSON_Print(root));
	send_message_by_username(username, jsonstr);
}


int send_group_message_callback(void *arg, int nr, char **values, char **names){
	printf("send_group_message_callback %s\n", values[0]);
	send_message_by_username(values[0], copy_string((char *)arg));
   	return 0;
}



void send_group_message(char *message) {
	cJSON *root = cJSON_Parse(message);
	int groupID = cJSON_GetObjectItem(root, "sendto")->valueint;
	char *sendfrom = cJSON_GetObjectItem(root, "sendfrom")->valuestring;
	char *content = cJSON_GetObjectItem(root, "content")->valuestring;
	char sqlcmd[2048];
	memset(sqlcmd, '\0', sizeof(sqlcmd));
	sprintf(sqlcmd, "select member from groupmember where groupID=%d", groupID);
	sqlite3_exec(db, sqlcmd, send_group_message_callback, message, NULL);
}



void send_group_profile(char *requested_user, int groupID) {
	//printf("sending group profile for %s, %d\n", requested_user, groupID);
	char sqlcmd[1024];
	memset(sqlcmd, '\0', sizeof(sqlcmd));
	sprintf(sqlcmd, "select member from groupmember where groupID=\'%d\'", groupID);
	char **res; char *errmsg; int nrow; int ncol;
	if(SQLITE_OK != sqlite3_get_table(db, sqlcmd, &res, &nrow, &ncol, &errmsg)) {
		printf("error while reading group profile: %s\n", errmsg);
		return ;
	}
	cJSON *root = cJSON_CreateObject();
	cJSON *list = cJSON_CreateArray();

	char username[32];
	int i, j; int nindex = ncol;
	for(i=0;i<nrow;i++){
		for(j=0;j<ncol;j++){
			res[nindex];
			strcpy(username, res[nindex]);
			cJSON *item;
			cJSON_AddItemToArray(list, item = cJSON_CreateObject());
			cJSON_AddStringToObject(item, "username", copy_string(username));
			nindex++;
		}
	}
	cJSON_AddStringToObject(root, "type", "group-profile");
	cJSON_AddNumberToObject(root, "member-count", nrow);
    	cJSON_AddNumberToObject(root, "groupID", groupID);
	cJSON_AddItemToObject(root, "list", list);
	char *jsonstr = copy_string(cJSON_Print(root));
	send_message_by_username(requested_user, jsonstr);
}


void send_friend_image(char* username,char* sendfrom) {
	struct client_property * fri_prop = get_user_prop(sendfrom);
	char sqlcmd[2048];
	char **res; char *errmsg; int nrow, ncol;
	memset(sqlcmd, '\0', sizeof(sqlcmd));
	sprintf(sqlcmd, "select image from userimage where username=\'%s\'", username);
	if(SQLITE_OK != sqlite3_get_table(db, sqlcmd, &res, &nrow, &ncol, &errmsg)) {
		printf("error while login(select userimage): %s\n", errmsg);
		return ;
	}
	printf("select image-------- res[1]:%s\n",res[1]);
	int user_image = res[1][0] - '0';
	sprintf(sqlcmd, "{\"type\":\"friend-image-receipt\",\"image\":%d,\"sendto\":\"%s\",\"sendfrom\":\"%s\"}",user_image,username,sendfrom);
	printf("%s",sqlcmd);
	send_message_by_fd(fri_prop->client_fd, sqlcmd);
}



void user_image_modify(char *username, int user_image) {
	printf("---------------in user_image_modify: user_image:%d\n",user_image);
	char sqlcmd[2048];
	sprintf(sqlcmd, "update userimage set image=\'%d\' where username=\'%s\'", user_image, username);
	sqlite3_exec(db, sqlcmd, NULL, NULL, NULL);
}



void handle_client_message(struct client_property * prop, const char * message) {
	if(DEBUG) printf("handling message from client %s, message is\n%s\n",
		inet_ntoa(prop->addr.sin_addr), message);
	cJSON *root = cJSON_Parse(message);
	if(root == NULL) return ;
	char *type = cJSON_GetObjectItem(root, "type")->valuestring;
	char message_json[BUFFER_SIZE];
	if(strcmp(type, "register-message") == 0) {
		//TODO: the strings'space might be freed
		char *username = cJSON_GetObjectItem(root, "username")->valuestring;
		char *password = cJSON_GetObjectItem(root, "password")->valuestring;
		user_register(prop, username, password);
	} else if(strcmp(type, "login-message") == 0) {
		char *username = cJSON_GetObjectItem(root, "username")->valuestring;
		char *password = cJSON_GetObjectItem(root, "password")->valuestring;
		user_login(prop, username, password);
	} else if(strcmp(type, "logout-message") == 0) {
		char *username = cJSON_GetObjectItem(root, "username")->valuestring;
		user_logout(prop, username);
	} else if(strcmp(type, "message/text") == 0) {
		memset(message_json, '\0', sizeof(message_json));
		strcpy(message_json, message);
		send_single_message(message_json);
	} else if(strcmp(type,"message/file") == 0){
		memset(message_json, '\0', sizeof(message_json));
		strcpy(message_json, message);
		send_return_message(message_json);
		//新加的message_return;
	} else if(strcmp(type, "message/text/group") == 0) {
		memset(message_json, '\0', sizeof(message_json));
		strcpy(message_json, message);
		send_group_message(message_json);
	} else if(strcmp(type, "friend-list-request") == 0) {
		char *username = cJSON_GetObjectItem(root, "username")->valuestring;
		send_friend_list(username);
	} else if(strcmp(type, "add-to-contact-request") == 0) {
		char *username = cJSON_GetObjectItem(root, "username")->valuestring;
		char *contact = cJSON_GetObjectItem(root, "contact")->valuestring;
		add_contact(username, contact);
	} else if(strcmp(type, "group-create-request") == 0) {
		char *username = cJSON_GetObjectItem(root, "username")->valuestring;
		create_group(username);
	} else if(strcmp(type, "group-list-request") == 0) {
		char *username = cJSON_GetObjectItem(root, "username")->valuestring;
		send_group_list(username);
	} else if(strcmp(type, "group-profile-request") == 0) {
		char *username = cJSON_GetObjectItem(root, "username")->valuestring;
		int groupID = cJSON_GetObjectItem(root, "groupID")->valueint;
		send_group_profile(prop->username, groupID);
	} else if(strcmp(type, "group-join-request") == 0) {
		char *username = cJSON_GetObjectItem(root, "username")->valuestring;
		int groupID = cJSON_GetObjectItem(root, "groupID")->valueint;
		join_group(username, groupID);
	} else if(strcmp(type, "group-quit-request") == 0) {
		char *username = cJSON_GetObjectItem(root, "username")->valuestring;
		int groupID = cJSON_GetObjectItem(root, "groupID")->valueint;
		quit_group(username, groupID);
	} else if(strcmp(type, "friend-image-request") == 0) {
		char *username = cJSON_GetObjectItem(root, "friendname")->valuestring;
		char *sendfrom = cJSON_GetObjectItem(root, "myname")->valuestring;
		send_friend_image(username,sendfrom);
	} else if(strcmp(type, "image-modify-request") == 0) {
		char *username = cJSON_GetObjectItem(root, "username")->valuestring;
		int user_image = cJSON_GetObjectItem(root, "userimage")->valueint;
		user_image_modify(username, user_image);
	}
	free(root);
	printf("handle client message over\n");
}



int get_user_fd(char *username) {
	if(username == NULL) return -1;
	int i;
	for(i = 0; i < MAX_CLIENT_COUNT; i++) {
		if(strcmp(username, client_prop[i].username) == 0) {
			return client_prop[i].client_fd;
		}
	}
	return -1;
}



//============================SUB THREADS================================
void client_thread_function(void *arg) {
	struct client_property * prop = (struct client_property *) arg;
	//when in pressure test mode, send welcome message after some command rather than automatically.
	//if(!PRESSURE_TEST)
	//	send(prop->client_fd, welcome_message, sizeof(char) * (strlen(welcome_message) + 1), 0);
	printf("prop: %d %s\n", prop->client_fd, inet_ntoa(prop->addr.sin_addr));
	char buf[BUFFER_SIZE];
	int numbytes;

	while(1) {
		printf("recv.....\n");
		numbytes = recv(prop->client_fd, buf, BUFFER_SIZE, 0);
		if(0 >= numbytes) {
			printf("user %s is offline.\n", inet_ntoa(prop->addr.sin_addr));
			break;
		}
		buf[numbytes] = '\0';
		handle_client_message(prop, buf);
		//log when receive a message
		//sprintf(log_buffer, "recv %s\n", buf);
		//write_log(logfile, log_buffer);
		printf("FUCK YOU\n");
	}
	delete_client(prop);
	pthread_exit(NULL);
}



void listen_thread_function(void *arg) {
	int sin_size = sizeof(struct sockaddr_in);
	while(1) {
		if((connectfd=accept(listenfd,(struct sockaddr *)&client,&sin_size))==-1){
			perror("accept() error");
			exit(1);
		}
		printf("connection from %s\n", inet_ntoa(client.sin_addr));
		add_client(connectfd, client);
	}
}




void send_thread_function(void *arg) {
	struct send_prop msg;
	char send_buffer[BUFFER_SIZE];
	while(1) {
		pthread_mutex_lock(&queue_lock);
		if(send_q_head != send_q_tail) {
			msg = send_queue[send_q_head];
			send_q_head = (send_q_head + 1) % MAX_SEND_QUEUE_SIZE;
			int fd = get_user_fd(msg.username);
			if(fd != -1){
				if(DEBUG) printf("sending message to online user %s: %s\n", msg.username, msg.message);
                memset(send_buffer, '\0', sizeof(send_buffer));
                strcpy(send_buffer, msg.message);
				//log when sending message
				sprintf(log_buffer, "send %s\n", send_buffer);
				write_log(logfile, log_buffer);			
                //send every message in FIXED size
				send(fd, send_buffer, sizeof(send_buffer), 0);	
				//FIXME: free here???
				free(msg.message);
			}
			else {
				if(DEBUG) printf("saving offline message for user %s, message is %s\n", msg.username, msg.message);
				save_offline_message(msg);
			}
			pthread_mutex_unlock(&queue_lock);
		} else {
			pthread_mutex_unlock(&queue_lock);
			//sleep here to avoid high CPU usagelogin
			sleep(1);
		}
		
	}
}
//============================SUB THREADS================================



//=====================ONLINE & OFFLINE MESSAGE==========================
void save_offline_message(struct send_prop msg) {
	cJSON *root = cJSON_Parse(msg.message);
	char *type = cJSON_GetObjectItem(root, "type")->valuestring;
	//ALL MESSAGE SHOULD BE CHAT MESSAGE!!!
	if(!(strcmp(type, "message/text") == 0 || strcmp(type, "message/text/group") == 0)) {
		free(root);	
		return ;
	}
	free(root);
	char sqlcmd[4096];
	memset(sqlcmd, '\0', sizeof(sqlcmd));
	sprintf(sqlcmd, "insert into offlinemsg values(\'%s\', \'%s\')", msg.username, msg.message);
	sqlite3_exec(db, sqlcmd, NULL, NULL, NULL);
}


void send_offline_message(char *username) {
	printf("sending offline message to %s\n", username);
	char sqlcmd[4096];
	sprintf(sqlcmd, "select msg from offlinemsg where sendto=\'%s\'", username);
	char **res; char *errmsg; int nrow; int ncol;
	if(SQLITE_OK != sqlite3_get_table(db, sqlcmd, &res, &nrow, &ncol, &errmsg)) {
		printf("error while reading offline message: %s\n", errmsg);
		return ;
	}
	char message[BUFFER_SIZE];
	int i, j; int nindex = ncol;
	for(i=0;i<nrow;i++){
		for(j=0;j<ncol;j++){
			strcpy(message, res[nindex]);
			send_message_by_username(username, copy_string(message));
			nindex++;
		}
	}
	sprintf(sqlcmd, "delete from offlinemsg where sendto=\'%s\'", username);
	sqlite3_exec(db, sqlcmd, NULL, NULL, NULL);
}



void send_message_by_username(char *username, char *msg) {
	pthread_mutex_lock(&queue_lock);
	//printf("sending message to user %s, message is %s\n", username, message);
	if((send_q_tail + 1) % MAX_SEND_QUEUE_SIZE == send_q_head) return ;
	char *message = copy_string(msg);
	send_queue[send_q_tail].message = message;
	strcpy(send_queue[send_q_tail].username, username);
	send_q_tail = (send_q_tail + 1) % MAX_SEND_QUEUE_SIZE;
	free(msg);
	pthread_mutex_unlock(&queue_lock);
}


void send_message_by_fd(int fd, char *message){
    char send_buffer[2048];
    memset(send_buffer, '\0', sizeof(send_buffer));
    strcpy(send_buffer, message);
	sprintf(log_buffer, "send %s\n", send_buffer);
	write_log(logfile, log_buffer);
	send(fd, send_buffer, sizeof(send_buffer), 0);
}
//=====================ONLINE & OFFLINE MESSAGE==========================



void add_client(int connectfd, struct sockaddr_in addr) {
	int idx = 0;
	for(idx = 0; idx < MAX_CLIENT_COUNT; idx++) {
		if(client_prop[idx].client_fd == -1) {
			break;
		}
	}
	if(client_prop[idx].client_fd != -1) {
		printf("failed to create new thread for connect %s\n", inet_ntoa(addr.sin_addr));
		return ;
	}
	client_prop[idx].client_fd = connectfd;
	client_prop[idx].addr = addr;
	int tid;
	pthread_create(&tid, NULL, client_thread_function, &client_prop[idx]);
	printf("created new thread for connect %s\n", inet_ntoa(addr.sin_addr));
}


void delete_client(struct client_property *prop) {
	close(prop->client_fd);
	prop->client_fd = -1;
	prop->is_online = 0;
	memset(prop->username, '\0', sizeof(prop->username));
}


int init_socket() {
	if(-1 == (listenfd = socket(AF_INET, SOCK_STREAM, 0))) {
		perror("create socket");
		return 0;
	}
	int opt = SO_REUSEADDR;
	setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	bzero(&server, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_port = htons(LISTEN_PORT);
	server.sin_addr.s_addr = htonl(INADDR_ANY);
	if(-1 == bind(listenfd, (struct sockaddr *)(&server), sizeof(struct sockaddr))) {
		perror("bind");
		return 0;
	}
	if(-1 == listen(listenfd, BACKLOG)) {
		perror("listen");
		return 0;
	}
	return 1;
}



void show_server_status() {
	printf("server status:\n");
	printf("max client count: %d\n", MAX_CLIENT_COUNT);
	int i;
	for(i = 0; i < MAX_CLIENT_COUNT; i++){
		printf("client %d: %d %s\n", i, client_prop[i].client_fd, client_prop[i].username);
	}
}



void exec_cmd(char * command) {
	if(strcmp(command, "exit") == 0){
		exit_server();
	}
	if(strcmp(command, "status") == 0){
		show_server_status();
	}
	if(PRESSURE_TEST && (strcmp(command, "welcome") == 0)) {
		int i;
		for(i = 0; i < MAX_CLIENT_COUNT; i++){
			if(client_prop[i].client_fd != -1){
				send_message_by_fd(client_prop[i].client_fd, welcome_message);
			}
		}
	}
}



void exit_server() {
	pthread_mutex_destroy(&queue_lock);
	exit(0);
}




void init_server() {
	//initialize data structures
	send_q_head = send_q_tail = 0;
	memset(send_queue, 0, sizeof(send_queue));
	memset(client_prop, 0, sizeof(client_prop));
	int i;
	for(i = 0; i < MAX_CLIENT_COUNT; i++)
		client_prop[i].client_fd = -1;
	
	//initialize multithread lock
	pthread_mutex_init(&queue_lock, NULL);
	
	//initialize database	
	if(!db_init()){
		printf("database initialization failed.\n");
		return 0;
	}
	
	//initialize log file
	if(LOG_ON){
		char logfilename[64];
		sprintf(logfilename, "server log - %s.log", get_formatted_time());
		logfile = create_log_file(logfilename);
	}
	
	//initialize network
	if(!init_socket()) {
		printf("error while initializing socket.\n");
		return 0;
	}
}
