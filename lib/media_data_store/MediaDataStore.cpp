﻿/**
 *  File: ap_airplay_service.cpp
 *  Project: apsdk
 *  Created: Oct 25, 2018
 *  Author: Sheen Tian
 *  
 *  This file is part of apsdk (https://github.com/air-display/apsdk-public) 
 *  Copyright (C) 2018-2024 Sheen Tian 
 *  
 *  apsdk is free software: you can redistribute it and/or modify it under the terms 
 *  of the GNU General Public License as published by the Free Software Foundation, 
 *  either version 3 of the License, or (at your option) any later version.
 *  
 *  apsdk is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; 
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. 
 *  See the GNU General Public License for more details.
 *  
 *  You should have received a copy of the GNU General Public License along with this. 
 *  If not, see <https://www.gnu.org/licenses/>.
 *==============================================================================
 * modified by fduncanh (2024)
 * based on class ap_casting_media_data_store of
 * http://github.com/air-display/apsdk-public
 */

#include <fstream>
#include <regex>
#include <stdio.h>

#include  "../hlsparse/hlsparse.h"
#include  "MediaDataStore.h"

#include <plist/plist.h>
#include "../http_response.h"
#include "../compat.h"

void send_fcup_request(const char *url, int request_id, char *client_session_id, int socket_fd) {
    /* values taken from apsdk-public;  */
  
    /* these seem to be arbitrary choices */
    const int sessionID = 1;
    const int FCUP_Response_ClientInfo = 1;
    const int FCUP_Response_ClientRef = 40030004;

    /* taken from a working AppleTV? */
    const char User_Agent[] = "AppleCoreMedia/1.0.0.11B554a (Apple TV; U; CPU OS 7_0_4 like Mac OS X; en_us";

    plist_t session_id_node = plist_new_int((int64_t) sessionID);
    plist_t type_node = plist_new_string("unhandledURLRequest");
    plist_t client_info_node = plist_new_int(FCUP_Response_ClientInfo);
    plist_t client_ref_node = plist_new_int((int64_t) FCUP_Response_ClientRef);
    plist_t request_id_node = plist_new_int((int64_t) request_id);
    plist_t url_node = plist_new_string(url);
    plist_t playback_session_id_node = plist_new_string(client_session_id);
    plist_t user_agent_node = plist_new_string(User_Agent);
    
    plist_t req_root_node = plist_new_dict();
    plist_dict_set_item(req_root_node, "sessionID", session_id_node);
    plist_dict_set_item(req_root_node, "type", type_node);

    plist_t fcup_request_node = plist_new_dict();
    plist_dict_set_item(fcup_request_node, "FCUP_Response_ClientInfo", client_info_node);
    plist_dict_set_item(fcup_request_node, "FCUP_Response_ClientRef", client_ref_node);
    plist_dict_set_item(fcup_request_node, "FCUP_Response_RequestID", request_id_node);
    plist_dict_set_item(fcup_request_node, "FCUP_Response_URL", url_node);
    plist_dict_set_item(fcup_request_node, "SessionID", session_id_node);
				    
    plist_t fcup_response_header_node = plist_new_dict();
    plist_dict_set_item(fcup_response_header_node, "X-Playback-Session-ID", playback_session_id_node);
    plist_dict_set_item(fcup_response_header_node, "User-Agent", user_agent_node);

    plist_dict_set_item(fcup_request_node, "FCUP_Response_Header", fcup_response_header_node);
    plist_dict_set_item(req_root_node, "request", fcup_request_node);
    
    uint32_t uint_val;
    char *plist_xml;
    plist_to_xml(req_root_node, &plist_xml, &uint_val);
    int datalen = (int) uint_val;
			  
    /* use tools for creating http responses to create the reverse http request */
    http_response_t *request = http_response_init_with_codestr("POST", "/event", "HTTP/1.1");
    http_response_add_header(request, "X-Apple-Session-ID", client_session_id);
    http_response_add_header(request, "Content-Type", "text/x-apple-plist+xml");
    http_response_finish(request, plist_xml, datalen);
    
    int len;
    const char *reverse_request = http_response_get_data(request, &len);
    int send_len = send(socket_fd, reverse_request, len, 0);
    if (send_len < 0) {
        int sock_err = SOCKET_GET_ERROR();
	fprintf(stderr, "send_fcup_request: error sending request. Error %d:%s",
                   sock_err, strerror(sock_err));
    }
   
    plist_free(req_root_node);   
    free (plist_xml);
}


std::string string_replace(const std::string &str, const std::string &pattern, const std::string &with) {
  std::regex p(pattern);
  return std::regex_replace(str, p, with);
}

#define PERSIST_STREAM_DATA 0

#if PERSIST_STREAM_DATA
#include <filesystem>
void create_session_folder(const std::string &session) { std::filesystem::create_directory(session); }

void create_resource_file(const std::string &session, const std::string &uri, const std::string &data) {
  std::string fn = generate_file_name();
  fn += string_replace(uri, "/|\\\\", "-");
  std::string path = session + "/" + fn;
  std::ofstream ofs;
  ofs.open(path, std::ofstream::out | std::ofstream::app);
  ofs << data;
  ofs.close();
}
#endif


MediaDataStore &MediaDataStore::get() {
  static MediaDataStore s_instance;
  return s_instance;
}

MediaDataStore::MediaDataStore() :  app_id_(e_app_unknown), request_id_(0), start_pos_in_ms_(0.0f), socket_fd_(0)
{  hlsparse_global_init();}

MediaDataStore::~MediaDataStore() = default;


void MediaDataStore::set_store_root(uint16_t port, int socket_fd) {
  std::ostringstream oss;
  oss << "localhost:" << port;
  host_ = oss.str();
  socket_fd_ = socket_fd;
}

bool MediaDataStore::request_media_data(const std::string &primary_uri, const std::string &session_id) {
  reset();

  app_id id = get_appi_id(primary_uri);

  if (id != e_app_unknown) {
#if PERSIST_STREAM_DATA
    create_session_folder(session_id);
#endif
    app_id_ = id;
    session_id_ = session_id;
    primary_uri_ = adjust_primary_uri(primary_uri);
    send_fcup_request(primary_uri.c_str(), request_id_++, session_id_.c_str(), socket_fd_);
    return true;
  }

  // Not local m3u8 uri
  return false;
}


// called from POST /action handler
std::string MediaDataStore::process_media_data(const std::string &uri, const char *data, int datalen) {

  std::string media_data;

  if (is_primary_data_uri(uri)) {
    master_t master_playlist;
    if (HLS_OK == hlsparse_master_init(&master_playlist)) {
      if (hlsparse_master(data, datalen, &master_playlist)) {

        // Save all media uri
        media_list_t *media_item = &master_playlist.media;
        while (media_item && media_item->data) {
          uri_stack_.push(media_item->data->uri);
          media_item = media_item->next;
        }

        // Save all stream uri
        stream_inf_list_t *stream_item = &master_playlist.stream_infs;
        while (stream_item && stream_item->data) {
          uri_stack_.push(stream_item->data->uri);
          stream_item = stream_item->next;
        }
      }
    }

    // Adjust the primary media data and cache it
    media_data = adjust_primary_media_data(data);
  } else {
    // Adjust the secondary media data and cache it
    media_data = adjust_secondary_media_data(data);
  }

  std::string path = extract_uri_path(uri);

  if (!path.empty() && !media_data.empty()) {
    add_media_data(path, media_data);
  }

  if (uri_stack_.empty()) {
    // no more data
    return primary_uri_;
  }

  auto next_uri = uri_stack_.top();
  uri_stack_.pop();
  send_fcup_request(next_uri.c_str(), request_id_++, session_id_.c_str(),  socket_fd_);

  return std::string();
}

std::string MediaDataStore::query_media_data(const std::string &path) {
  std::lock_guard<std::mutex> l(mtx_);
  auto it = media_data_.find(path);
  if (it != media_data_.end()) {
    return it->second;
  }
  return std::string();
}

void MediaDataStore::reset() {
  app_id_ = e_app_unknown;
  request_id_ = 1;
  session_id_.clear();
  primary_uri_.clear();
  uri_stack_ = std::stack<std::string>();

  media_data_.clear();
}

MediaDataStore::app_id MediaDataStore::get_appi_id(const std::string &uri) {
  // Youtube
  if (0 == uri.find(MLHLS_SCHEME))
    return e_app_youtube;

  // Netflix
  if (0 == uri.find(NFHLS_SCHEME))
    return e_app_netflix;

  return e_app_unknown;
}

void MediaDataStore::add_media_data(const std::string &uri, const std::string &data) {
  {
    std::lock_guard<std::mutex> l(mtx_);
    media_data_[uri] = data;
  }

#if PERSIST_STREAM_DATA
  create_resource_file(session_id_, uri, data);
#endif
}

bool MediaDataStore::is_primary_data_uri(const std::string &uri) {
  if (strstr(uri.c_str(), MASTER_M3U8))
    return true;
  if (strstr(uri.c_str(), INDEX_M3U8))
    return true;

  return false;
}

std::string MediaDataStore::adjust_primary_uri(const std::string &uri) {
  std::string s = uri;
  s = string_replace(s, SCHEME_LIST, HTTP_SCHEME);
  s = string_replace(s, HOST_LIST, host_);
  return s;
}

std::string MediaDataStore::extract_uri_path(const std::string &uri) {
  std::string s = uri;
  switch (app_id_) {
  case e_app_youtube:
    s = string_replace(s, MLHLS_SCHEME, "");
    s = string_replace(s, HOST_LIST, "");
  case e_app_netflix:
    s = string_replace(s, NFHLS_SCHEME, "");
    s = string_replace(s, HOST_LIST, "");
    if (s.at(0) != '/') {
      s = "/" + s;
    }
  default:
    break;
  }
  return s;
}

std::string MediaDataStore::adjust_primary_media_data(const std::string &data) {
  switch (app_id_) {
  case e_app_youtube:
    return adjust_mlhls_data(data);
  case e_app_netflix:
    return adjust_nfhls_data(data);
  default:
    break;
  }
  return data;
}

std::string MediaDataStore::adjust_secondary_media_data(const std::string &data) {
  std::string result = data;

  static std::regex youtube_pattern("#YT-EXT-CONDENSED-URL:BASE-URI=\"(.*)\",PARAMS=.*PREFIX=\"(.*)\"");
  std::cmatch groups;

  std::string base;
  std::string prefix;
  if (std::regex_search(result.c_str(), groups, youtube_pattern)) {
    if (groups.size() > 2) {
      base = groups.str(1);
      prefix = groups.str(2);
    }
  }

  if (!base.empty() && !prefix.empty()) {
    std::regex re("\n" + prefix);
    std::string fmt = "\n" + base + "/" + prefix;
    result = std::regex_replace(result, re, fmt);
  }

  return result;
}

std::string MediaDataStore::adjust_mlhls_data(const std::string &data) {
  std::string s = data;
  s = string_replace(s, MLHLS_SCHEME, HTTP_SCHEME);
  s = string_replace(s, HOST_LIST, host_);
  return s;
}

std::string MediaDataStore::adjust_nfhls_data(const std::string &data) {
  std::string s = data;
  std::string replace = HTTP_SCHEME;
  replace += host_;
  replace += "/";
  s = string_replace(s, NFHLS_SCHEME, replace);
  return s;
}

// wrappers for the public functions of class MediaDataStore (callable from C): 

//create the media_data_store, return a pointer to it.
extern "C" void* media_data_store_create(uint16_t port, int socket_fd) {
    MediaDataStore *media_data_store = new MediaDataStore;
    media_data_store->set_store_root(port, socket_fd);
    return (void *) media_data_store;
}

//delete the media_data_store
extern "C" void media_data_store_destroy(void *media_data_store) {
    delete static_cast<MediaDataStore*>(media_data_store);
}


// called by the POST /action handler:
extern "C" char *process_media_data(void *media_data_store, const char *url, const char *data, int datalen) {
    const std::string uri(url);
    auto location = static_cast<MediaDataStore*>(media_data_store)->process_media_data(uri, data, datalen) ;
    if (!location.empty()) {
        size_t len = location.length();
        char * location_str = (char *) malloc(len + 1);
        snprintf(location_str, len + 1, location.c_str());
        location_str[len] = '\0';
        return location_str; //this needs to be freed 
    }
    return NULL;
}

//called by the POST /play handler
extern "C" bool request_media_data(void *media_data_store, const char *primary_url, const char * session_id_in) {
    const std::string primary_uri = primary_url;
    const std::string session_id = session_id_in;
    return static_cast<MediaDataStore*>(media_data_store)->request_media_data(primary_uri, session_id);
}


//called by airplay_video_media_http_connection::get_handler:   &path = req.uri)
extern "C" int  query_media_data(void *media_data_store, const char *url, const char **media_data) {
    const std::string path = url;
    auto data =  static_cast<MediaDataStore*>(media_data_store)->query_media_data(path);
    if (data.empty()) {
        return 0;
    }
    size_t len = data.length();
    *media_data = data.c_str();
    return (int) len;
}

//called by the post_stop_handler:
extern "C" void media_data_store_reset(void *media_data_store) {
    static_cast<MediaDataStore*>(media_data_store)->reset();
}

// set and get session_id_ and start_pos_in_ms_

extern "C" const char *get_session_id(void *media_data_store) {
    return static_cast<MediaDataStore*>(media_data_store)->get_session_id();
}

extern "C" void set_session_id(void *media_data_store, const char *session_id) {
    static_cast<MediaDataStore*>(media_data_store)->set_session_id(session_id);
}

extern "C" const char *get_plackback_uuid(void *media_data_store) {
    return static_cast<MediaDataStore*>(media_data_store)->get_playback_uuid();
}

extern "C" void set_playback_uuid(void *media_data_store, const char *playback_uuid) {
    static_cast<MediaDataStore*>(media_data_store)->set_playback_uuid(playback_uuid);
}

extern "C" float get_start_pos_in_ms(void *media_data_store) {
    return static_cast<MediaDataStore*>(media_data_store)->get_start_pos_in_ms();
}

extern "C" void set_start_pos_in_ms(void *media_data_store, float start_pos_in_ms) {
    static_cast<MediaDataStore*>(media_data_store)->set_start_pos_in_ms(start_pos_in_ms);
}



//unused
#if 0
bool get_youtube_url(const char *data, uint32_t length, std::string &url) {
  static std::regex pattern("#YT-EXT-CONDENSED-URL:BASE-URI=\"(.*)\",PARAMS=");
  std::cmatch groups;

  if (std::regex_search(data, groups, pattern)) {
    if (groups.size() > 1) {
      url = groups.str(1);
      return true;
    }
  }

  return false;
}

std::string get_best_quality_stream_uri(const char *data, uint32_t length) {
  HLSCode r = hlsparse_global_init();
  master_t master_playlist;
  r = hlsparse_master_init(&master_playlist);
  r = hlsparse_master(data, length, &master_playlist);
  stream_inf_list_t *best_quality_stream = 0;
  stream_inf_list_t *stream_inf = &master_playlist.stream_infs;
  return master_playlist.media.data->uri;
  while (stream_inf && stream_inf->data) {
    if (!best_quality_stream) {
      best_quality_stream = stream_inf;
    } else if (stream_inf->data->bandwidth > best_quality_stream->data->bandwidth) {
      best_quality_stream = stream_inf;
    }
    stream_inf = stream_inf->next;
  }
  if (best_quality_stream) {
    return best_quality_stream->data->uri;
  }

  return std::string();
}
#endif
