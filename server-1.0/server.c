#include <event.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <err.h>
#include <lua.h>
#include <lauxlib.h>     
#include <lualib.h> 
#include <string.h>
#include "parson.h"
#include <errno.h>
#include <dirent.h>

struct lua_ptr{
	char *name;
	lua_State * LS;
	struct lua_ptr * next;
};

typedef struct lua_ptr lua_ptr_entry ;

#define SERVER_PORT 9005
#define SUCCESS 0
#define FAILURE -1

lua_ptr_entry * lpe_list;

lua_State * createLS(char *file)
{
	char path[256];
	sprintf(path,"./script/%s",file);
	lua_State * L;
	L = luaL_newstate();
	luaL_openlibs(L);
	luaL_dofile(L, path);
	printf("LUA=> %s has been loaded.\r\n",path);
	return L;
}

void iniLuaStates(lua_ptr_entry **lpe)
{
	DIR    *dir;
	struct    dirent    *ptr;
	lua_ptr_entry *p,*c;
	dir = opendir("./script/");
	*lpe=(lua_ptr_entry *)malloc(sizeof(lua_ptr_entry));
	(*lpe)->name="__root";
	(*lpe)->LS=NULL;
	(*lpe)->next=NULL;
	p = *lpe;
	while((ptr = readdir(dir)) != NULL) 
	{
		if(strcmp(".",ptr->d_name)!=0&&strcmp("..",ptr->d_name)){
			while(p){
				if(p->next==NULL)
					break;
				p=p->next;
			}
			c=(lua_ptr_entry *)malloc(sizeof(lua_ptr_entry));
			c->name = ptr->d_name;
			c->LS =createLS(ptr->d_name); 
			c->next = p->next;
			p->next = c;
		}
	}

	// c=f;

	// while(c)
	//{
	//        printf("%s\r\n",c->name);
	//        c = c->next;
	// }

	closedir(dir);
}

lua_State * findState(const char *file,lua_ptr_entry *lpe){
	char path[256];
        sprintf(path,"%s.lua",file);

	while(lpe)
	{
		if(strcmp(lpe->name,path)==0){
			return lpe->LS;
		}
		lpe = lpe->next;
	}
	return NULL;
}


int debug = 0;
extern int errno ;
lua_State * L=NULL;

struct client {
	int fd;
	struct bufferevent *buf_ev;
};

void setnonblock(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL);
	flags |= O_NONBLOCK;
	fcntl(fd, F_SETFL, flags);
}

const char * call_lua(lua_State * L,const char *func,const char *json)
{
	const char * result;
	lua_getglobal(L,func);  
	lua_pushstring(L,json);  
	lua_pcall(L,1,1,0);  
	result = lua_tostring(L,-1);  
	return result;  
}

void buf_read_callback(struct bufferevent *incoming, void *arg)
{
	struct evbuffer *evreturn;
	char *req;
	const char * file,*func,*params;
	int i =0;
	while(1){
		req = evbuffer_readline(incoming->input);
		if(i==4) break;
		i++;
	}

	JSON_Value *schema = json_parse_string(req);
	file =json_object_get_string(json_object(schema),"f");
	func =json_object_get_string(json_object(schema),"m");	
	params = json_object_get_string(json_object(schema),"p");

	printf("request => [file: ./script/%s.lua , func: %s , params: %s]\r\n",file,func,params);

	const char * result;
	lua_State * L = findState(file,lpe_list);
	if(L==NULL){
		result ="LUA invoked failure.";
	}
	else{
		result = call_lua(L,func,params);
	}
	printf("response =>[ result: %s]\r\n",result);
	
	evreturn = evbuffer_new();
	evbuffer_add_printf(evreturn,"+ok$%ld$%s\r\n",sizeof(result),result);
	//evbuffer_add_printf(evreturn,"+ok\r\n");
	//printf("evbuffer_add_printf error : %s",strerror(errno));

	if(bufferevent_write_buffer(incoming,evreturn)!=SUCCESS){
		printf("evbuffer_add_printf error : %s",strerror(errno));
	}

	evbuffer_free(evreturn);
	free(req);
}

void buf_write_callback(struct bufferevent *bev,void *arg)
{
}

void buf_error_callback(struct bufferevent *bev,short what,void *arg)
{
	struct client *client = (struct client *)arg;
	bufferevent_free(client->buf_ev);
	close(client->fd);
	free(client);
}

void accept_callback(int fd,short ev,void *arg)
{
	int client_fd;
	struct sockaddr_in client_addr;
	socklen_t client_len = sizeof(client_addr);
	struct client *client;

	client_fd = accept(fd,
			(struct sockaddr *)&client_addr,
			&client_len);
	if (client_fd < 0)
	{
		warn("Client: accept() failed");
		return;
	}

	setnonblock(client_fd);

	client = calloc(1, sizeof(*client));
	if (client == NULL)
		err(1, "malloc failed");
	client->fd = client_fd;

	client->buf_ev = bufferevent_new(client_fd,
			buf_read_callback,
			buf_write_callback,
			buf_error_callback,
			client);

	bufferevent_enable(client->buf_ev, EV_READ);
}

int main(int argc,char **argv)
{

	iniLuaStates(&lpe_list);

	int socketlisten;
	struct sockaddr_in addresslisten;
	struct event accept_event;
	int reuse = 1;

	event_init();

	socketlisten = socket(AF_INET, SOCK_STREAM, 0);

	if (socketlisten < 0)
	{
		fprintf(stderr,"Failed to create listen socket");
		return 1;
	}

	memset(&addresslisten, 0, sizeof(addresslisten));

	addresslisten.sin_family = AF_INET;
	addresslisten.sin_addr.s_addr = INADDR_ANY;
	addresslisten.sin_port = htons(SERVER_PORT);

	if (bind(socketlisten,
				(struct sockaddr *)&addresslisten,
				sizeof(addresslisten)) < 0)
	{
		fprintf(stderr,"Failed to bind");
		return 1;
	}

	if (listen(socketlisten, 5) < 0)
	{
		fprintf(stderr,"Failed to listen to socket");
		return 1;
	}

	setsockopt(socketlisten,
			SOL_SOCKET,
			SO_REUSEADDR,
			&reuse,
			sizeof(reuse));

	setnonblock(socketlisten);

	event_set(&accept_event,
			socketlisten,
			EV_READ|EV_PERSIST,
			accept_callback,
			NULL);

	event_add(&accept_event,
			NULL);

	event_dispatch();

	close(socketlisten);

	return 0;
}