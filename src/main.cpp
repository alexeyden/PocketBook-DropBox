#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "cJSON.h"
#include "inkview.h"

#include <cstring>
#include <iostream>
#include <sstream>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <pthread.h>

using std::string;
using std::vector;


struct CurlChunk {
	char* memory;
	size_t size;
};

class Curl {
public:
    Curl() {
        curl_global_init(CURL_GLOBAL_ALL);
        m_curl = curl_easy_init();
    }
    ~Curl() {
        curl_easy_cleanup(m_curl);
        curl_global_cleanup();
    }
    
    string post(const string& url, const string& data, const vector<string>& headers) {
        curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYHOST, 0L);
        
        struct CurlChunk mem;
        mem.memory = (char*) malloc(1);
        mem.size = 0;
        
        curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, &write_callback);
        curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, (void *)&mem);
        
        struct curl_slist *hdr= NULL;
        for(int i = 0; i < headers.size(); i++)
            hdr = curl_slist_append(hdr, headers[i].c_str());
        
        curl_easy_setopt(m_curl, CURLOPT_HTTPHEADER, hdr);
        
        curl_easy_setopt(m_curl, CURLOPT_URL, url.c_str());
        
        if(data != "")
            curl_easy_setopt(m_curl, CURLOPT_POSTFIELDS, data.c_str());
        
        CURLcode res = curl_easy_perform(m_curl);
        
        if(res != CURLE_OK) {
            throw std::runtime_error("Curl post failed:" + string(curl_easy_strerror(res)));
        }
        else {
            string result(mem.memory);
            free(mem.memory);
            return result;
        }
    }
    
    void download(const string& url, const vector<string>& headers, FILE* fp) {
        curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYHOST, 0L);
        
        curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, &file_write_callback);
        curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, (void *)fp);
        
        struct curl_slist *hdr= NULL;
        for(int i = 0; i < headers.size(); i++)
            hdr = curl_slist_append(hdr, headers[i].c_str());
        
        curl_easy_setopt(m_curl, CURLOPT_HTTPHEADER, hdr);
        curl_easy_setopt(m_curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(m_curl, CURLOPT_POSTFIELDS, "");
        
        CURLcode res = curl_easy_perform(m_curl);
        
        if(res != CURLE_OK) {
            throw std::runtime_error("Curl post failed:" + string(curl_easy_strerror(res)));
        }
    }
    
private:
    static size_t file_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
        FILE* fp = (FILE*) userp;
        
        return fwrite(contents, size, nmemb, fp);
    }
    
    static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp)
    {
        size_t realsize = size * nmemb;
        struct CurlChunk* mem = (struct CurlChunk*) userp;
        
        mem->memory = (char*) realloc(mem->memory, mem->size + realsize + 1);
        
        memcpy(&(mem->memory[mem->size]), contents, realsize);
        mem->size += realsize;
        mem->memory[mem->size] = 0;
        
        return realsize;
    }
    
    CURL *m_curl;
};

struct DropBoxFileItem {
    string file;
    string id;
};

class DropBox {
public:
    DropBox(string key) : m_key(key) {
    }
    
    std::vector<DropBoxFileItem> listFiles(string folder) {
        string json = postListFiles(folder);
        
        vector<DropBoxFileItem> files;
        string cursor;
        
        if(parseFileList(json, files, cursor)) {
            json = postListFilesContinue(cursor);
            
            while(parseFileList(json, files, cursor)) {}
        }
        
        return files;
    }
    
    void download(string id, string outpath) {
        vector<string> headers;
        headers.push_back("Authorization: Bearer " + m_key);
        headers.push_back("Dropbox-API-Arg: {\"path\": \"" + id + "\"}");
        headers.push_back("Content-Type:");
        
        string url = "https://content.dropboxapi.com/2/files/download";
        
        FILE* fp = fopen(outpath.c_str(), "w");
        m_curl.download(url, headers, fp);
        fclose(fp);
    }
    
private:
    string postListFiles(string folder) {
        vector<string> headers;
        headers.push_back("Authorization: Bearer " + m_key);
        headers.push_back("Content-Type: application/json");
        
        string url = "https://api.dropboxapi.com/2/files/list_folder";
        string data = "{\"path\": \"" +
            folder +
            "\",\"recursive\": false,\"include_media_info\": false,\"include_deleted\": false,\"include_has_explicit_shared_members\": false}";
        
        string result = m_curl.post(url, data, headers);
        
        return result;
    }
    
    string postListFilesContinue(string cursor) {
        vector<string> headers;
        headers.push_back("Authorization: Bearer " + m_key);
        headers.push_back("Content-Type: application/json");
        
        string url = "https://api.dropboxapi.com/2/files/list_folder/continue";
        string data = "{\"cursor\": \"" + cursor + "\"}";
        
        string result = m_curl.post(url, data, headers);
        
        return result;
    }
    
    bool parseFileList(const string& json, vector<DropBoxFileItem>& items, string& cursor) {
        cJSON * root = cJSON_Parse(json.c_str());
        
        if(root == NULL)
            throw std::runtime_error("Invalid JSON:\n" + json);
        
        cJSON * ent = cJSON_GetObjectItem(root, "entries");
        cJSON * more = cJSON_GetObjectItem(root, "has_more");
        cJSON * jcursor = cJSON_GetObjectItem(root, "cursor");
        
        if(ent == NULL || more == NULL || jcursor == NULL)
            throw std::runtime_error("Invalid JSON:\n" + json);
        
        int size = cJSON_GetArraySize(ent);
        for(int i = 0; i < size; i++) {
            if(cJSON_GetObjectItem(cJSON_GetArrayItem(ent, i), ".tag")->valuestring != string("file"))
                continue;
            
            DropBoxFileItem item = {
                cJSON_GetObjectItem(cJSON_GetArrayItem(ent, i), "name")->valuestring,
                cJSON_GetObjectItem(cJSON_GetArrayItem(ent, i), "id")->valuestring
            };
            
            items.push_back(item);
        }
        
        cJSON_Delete(root);
        
        cursor = jcursor->valuestring;
        
        return more->type == cJSON_True;
    }
    
    Curl m_curl;
    string m_key;
};

class Application {
public:
    Application(string key, string path) : m_key(key), m_path(path) {
        pthread_mutex_init(&mutex_log, NULL);
        ifont* menu_font = GetThemeFont("menu.font.normal", "");
        SetFont(const_cast<ifont*>(menu_font), BLACK);
    }
    
    ~Application() {
        stop();
        pthread_mutex_destroy(&mutex_log);
    }
    
    void redraw() {
		//ClearScreen();
        pthread_mutex_lock(&mutex_log);
        DrawTextRect(10, 10, ScreenWidth() - 20, ScreenHeight() - 20, m_log.c_str(), ALIGN_LEFT);
        pthread_mutex_unlock(&mutex_log);
		DynamicUpdateBW(10, 10, ScreenWidth() - 20, ScreenHeight() - 20);
	}
    
    int event(int event, int param1, int param2) {
		switch(event) {
			case EVT_SHOW:
				redraw();
				break;
			default:
				break;
		}

		return 0;
	}
	
	void start() {
        pthread_create(&thread, NULL, &worker, this); 
    }
    
    void stop() {
        pthread_cancel(thread);
    }
	
private:
	static void* worker(void *data) {
        Application* self = (Application*) data;
        
        DropBox db(self->m_key);
        
        vector<DropBoxFileItem> items;
        
        try {
            items = db.listFiles("");
        } catch(std::exception e) {
            pthread_mutex_lock(&self->mutex_log);
            self->m_log += string("ERROR: ") + e.what() + "\n";
            pthread_mutex_unlock(&self->mutex_log);
            
            return NULL;
        }
    
        for(int i = 0; i < items.size(); i++) {
            bool exists = false;
            FILE* fp = fopen((self->m_path + "/" + items[i].file).c_str(), "r");
            if(fp != NULL) {
                fclose(fp);
                exists = true;
            }
            
            pthread_mutex_lock(&self->mutex_log);
            self->m_log += "File: " + items[i].file + " (" + (exists ? "exists" : "downloading...") + ")" + "\n";
            pthread_mutex_unlock(&self->mutex_log);
            
            self->redraw();
            
            if(!exists) {
                try {
                    db.download(items[i].id, self->m_path + "/" + items[i].file);
                } catch(std::exception e) {
                    pthread_mutex_lock(&self->mutex_log);
                    self->m_log += string("DOWNLOAD ERROR: ") + e.what() + "\n";
                    pthread_mutex_unlock(&self->mutex_log);
                    
                    return NULL;
                }
            }
        }
        
        pthread_mutex_lock(&self->mutex_log);
        self->m_log += "Complete\n";
        pthread_mutex_unlock(&self->mutex_log);
        
        self->redraw();
        
        return NULL;
    }
    
    pthread_mutex_t mutex_log;
    pthread_t thread;
    
    string m_key;
    string m_path;
    string m_log;
};

Application* app = NULL;

int global_event_handler(int type, int param1, int param2) {
    std::ifstream t(CONFIGPATH "/DropBoxSync.json");
    
    if(!t.is_open()) {
        Message(ICON_ERROR, "Error", (string("No config found: ") + (CONFIGPATH "DropBoxSync.json")).c_str(), 5000);
        exit(1);
    }
    
    string data( (std::istreambuf_iterator<char>(t) ),
                       (std::istreambuf_iterator<char>()    ) );
    
    cJSON* cfg = cJSON_Parse(data.c_str());
    string key = cJSON_GetObjectItem(cfg, "api_key")->valuestring;
    string path = cJSON_GetObjectItem(cfg, "sync_dir")->valuestring;
    
    cJSON_Delete(cfg);
    
    
	switch(type) {
		case EVT_INIT:
			app = new Application(key, path);
            app->start();
			break;
		case EVT_SHOW:
			app->redraw();
			break;
		case EVT_EXIT:
			delete app;
			break;
		case EVT_KEYPRESS:
            CloseApp();
		default:
			app->event(type, param1, param2);
			break;
	}

	return 0;
}

int main(int, const char**) {
	InkViewMain(&global_event_handler);

	return 0;
}
