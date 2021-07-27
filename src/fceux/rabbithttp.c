/* rabbithttp.c, by Greg Cannon as a part of the StackRabbit AI project.
 * Based on hellofunc.c (C) 2011 by Steve Litt
 *
 * Command to compile on OSX:
 * 		gcc rabbithttp.c -Wall -shared -fPIC -o rabbithttp.so -I/usr/local/include/lua5.1 -llua5.1 -lcurl
 * 
 * Note the word "rabbithttp" matches the string after the underscore in
 * function luaopen_rabbithttp(). This is a must.
 * 
 * The -shared arg lets it compile to .so format.
 * The -fPIC is for certain situations and harmless in others.
 * On your computer, the -I and -l args will probably be different.
 *
 * ------------------------
 * Disclaimer: I don't know anything about networking in C so the libcurl and string management parts are
 * copied from StackOverflow.
 */

// Mandatory libraries for Lua modules
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

// Libraries for the HTTP part
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

struct string {
  char *ptr;
  size_t len;
};

/**
 * Initializes a copy of our custom string implementation
 */
void init_string(struct string *s) {
  s->len = 0;
  s->ptr = malloc(s->len+1);
  if (s->ptr == NULL) {
    fprintf(stderr, "malloc() failed\n");
    exit(EXIT_FAILURE);
  }
  s->ptr[0] = '\0';
}

/** 
 * Helper function to append data to the end of our custom string implementation 
 */
size_t writefunc(void *ptr, size_t size, size_t nmemb, struct string *s)
{
  size_t new_len = s->len + size*nmemb;
  s->ptr = realloc(s->ptr, new_len+1);
  if (s->ptr == NULL) {
    fprintf(stderr, "realloc() failed\n");
    exit(EXIT_FAILURE);
  }
  memcpy(s->ptr+s->len, ptr, size*nmemb);
  s->ptr[new_len] = '\0';
  s->len = new_len;

  return size*nmemb;
}

/** 
 * Makes an HTTP GET request to the provided URL, and returns the response as a string. 
 */
static int ihttpfetch(lua_State *L){              /* Internal name of func */
	const char *requestUrl = lua_tostring(L, -1);

	CURL *curl;
  CURLcode res;
  curl = curl_easy_init();
  if(curl) {
    struct string s;
    init_string(&s);

    curl_easy_setopt(curl, CURLOPT_URL, requestUrl);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
    
    res = curl_easy_perform(curl);
		lua_pushstring(L, s.ptr);
    free(s.ptr);

    /* always cleanup */
    curl_easy_cleanup(curl);
  } else {
		lua_pushstring(L, "an error occurred");
	}

	return 1;                              /* One return value */
}

/** 
 * Much simpler function that shows the minimal implementation of a C function for Lua consumption 
 */
static int icube(lua_State *L){                /* Internal name of func */
	float rtrn = lua_tonumber(L, -1);      /* Get the single number arg */
	printf("Top of cube(), number=%f\n",rtrn);
	lua_pushnumber(L,rtrn*rtrn*rtrn);      /* Push the return */
	return 1;                              /* One return value */
}


/* Register this file's functions with the
 * luaopen_libraryname() function, where libraryname
 * is the name of the compiled .so output. In other words
 * it's the filename (but not extension) after the -o
 * in the cc command.
 *
 * So for instance, if your cc command has -o rabbithttp.so then
 * this function would be called luaopen_rabbithttp().
 *
 * This function should contain lua_register() commands for
 * each function you want available from Lua.
 *
*/
int luaopen_rabbithttp(lua_State *L){
	lua_register(
			L,               /* Lua state variable */
			"httpFetch",        /* func name as known in Lua */
			ihttpfetch          /* func name in this file */
			);  
	lua_register(L,"cube",icube);
	return 0;
}
