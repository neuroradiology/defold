#include <float.h>
#include <stdio.h>
#include <assert.h>
#include <ctype.h>
#include <string.h>

#include <emscripten.h>

#include <ddf/ddf.h>
#include <dlib/dstrings.h>
#include <dlib/hash.h>
#include <dlib/log.h>
#include <dlib/math.h>

#include "script.h"
#include "http_ddf.h"

#include "script_http.h"
#include "http_service.h"

extern "C"
{
#include <lua/lauxlib.h>
#include <lua/lualib.h>
}

#include "script_http_util.h"

namespace dmScript
{
    uint64_t g_Timeout = 0;

    typedef void (*OnLoad)(void* context, int status, void* content, int content_size, const char* headers);
    typedef void (*OnError)(void* context, int status);

    void HttpRequestAsync(const char* method, const char* url, const char* headers, void *arg, OnLoad ol, OnError oe, const void* send_data, int send_data_length)
    {
        int timeout  = (int) g_Timeout;
        EM_ASM_ARGS({
            var method = $0;
            var url = $1;
            var headers = $2;
            var arg = $3;
            var onload = $4;
            var onerror = $5;
            var send_data = $6;
            var send_data_length = $7;
            var timeout = $8;
            var xhr = new XMLHttpRequest();

            if (timeout > 0) {
                xhr.timeout = timeout / 1000.0;
            }

            function listener() {
                var resp_headers = xhr.getAllResponseHeaders();
                resp_headers = resp_headers.replace(new RegExp("\r", "g"), "");
                resp_headers += "\n";

                if (xhr.status != 0) {
                    // TODO: Investigate memory and ALLOC_STACK
                    // See also library_browser.js where malloc and free is used
                    var ab = new Uint8Array(xhr.response);
                    var b = allocate(ab, 'i8', ALLOC_STACK);
                    var resp_headers_buffer = allocate(intArrayFromString(resp_headers), 'i8', ALLOC_STACK);
                    Runtime.dynCall('viiiii', onload, [arg, xhr.status, b, ab.length, resp_headers_buffer]);
                } else {
                    Runtime.dynCall('vii', onerror, [arg, xhr.status]);
                }
            }
            xhr.onload = listener;
            xhr.onerror = listener;
            xhr.ontimeout = listener;
            xhr.open(Pointer_stringify(method), Pointer_stringify(url), true);
            // TODO: Doesn't work in node.js. Why? xhr2?
            xhr.responseType = 'arraybuffer';

            var headersArray = Pointer_stringify(headers).split("\n");
            for (var i = 0; i < headersArray.length; i++) {
                if (headersArray[i].trim() != "") {
                    var a = headersArray[i].split(":");
                    xhr.setRequestHeader(a[0], a[1]);
                }
            }

            if (send_data_length > 0) {
                xhr.send(HEAPU8.subarray(send_data, send_data + send_data_length));
            } else {
                xhr.send();
            }
        }, method, url, headers, arg, ol, oe, send_data, send_data_length, timeout);
    }

    static void SendResponse(const dmMessage::URL* requester, int status,
                             const char* headers, uint32_t headers_length,
                             const char* response, uint32_t response_length)
    {
        dmHttpDDF::HttpResponse resp;
        resp.m_Status = status;
        resp.m_Headers = (uint64_t) headers;
        resp.m_HeadersLength = headers_length;
        resp.m_Response = (uint64_t) response;
        resp.m_ResponseLength = response_length;

        resp.m_Headers = (uint64_t) malloc(headers_length);
        memcpy((void*) resp.m_Headers, headers, headers_length);
        resp.m_Response = (uint64_t) malloc(response_length);
        memcpy((void*) resp.m_Response, response, response_length);

        if (dmMessage::IsSocketValid(requester->m_Socket)) {
            dmMessage::Post(0, requester, dmHttpDDF::HttpResponse::m_DDFHash, 0, (uintptr_t) dmHttpDDF::HttpResponse::m_DDFDescriptor, &resp, sizeof(resp));
        } else {
            free((void*) resp.m_Headers);
            free((void*) resp.m_Response);
            dmLogWarning("Failed to return http-response. Requester deleted?");
        }
    }

    void OnHttpLoad(void* context, int status, void* content, int content_size, const char* headers)
    {
        dmMessage::URL* requester = (dmMessage::URL*) context;
        SendResponse(requester, status, headers, strlen(headers), (const char*) content, content_size);
        delete requester;
    }

    void OnHttpError(void* context, int status)
    {
        dmMessage::URL* requester = (dmMessage::URL*) context;
        SendResponse(requester, status, 0, 0, 0, 0);
    }

    int Http_Request(lua_State* L)
    {
        int top = lua_gettop(L);

        dmMessage::URL sender;
        uintptr_t user_data;
        if (dmScript::GetURL(L, &sender) && dmScript::GetUserData(L, &user_data) && user_data != 0) {

            const char* url = luaL_checkstring(L, 1);
            const char* method = luaL_checkstring(L, 2);
            luaL_checktype(L, 3, LUA_TFUNCTION);
            lua_pushvalue(L, 3);
            // NOTE: + 2 as LUA_NOREF is defined as - 2 and 0 is interpreted as uninitialized
            int callback = luaL_ref(L, LUA_REGISTRYINDEX) + 2;
            sender.m_Function = callback;

            dmArray<char> h;
            h.SetCapacity(4 * 1024);
            if (top > 3) {

                lua_pushvalue(L, 4);
                lua_pushnil(L);
                while (lua_next(L, -2)) {
                    const char* attr = lua_tostring(L, -2);
                    const char* val = lua_tostring(L, -1);
                    uint32_t left = h.Capacity() - h.Size();
                    uint32_t required = strlen(attr) + strlen(val) + 2;
                    if (left < required) {
                        h.OffsetCapacity(dmMath::Max(required, 1024U));
                    }
                    h.PushArray(attr, strlen(attr));
                    h.Push(':');
                    h.PushArray(val, strlen(val));
                    h.Push('\n');
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);
            }
            h.Push('\0');

            char* request_data = 0;
            int request_data_length = 0;
            if (top > 4) {
                size_t len;
                const char* r = luaL_checklstring(L, 5, &len);
                request_data = (char*) malloc(len);
                memcpy(request_data, r, len);
                request_data_length = len;
            }

            dmMessage::URL* requester = new dmMessage::URL;
            *requester = sender;

            HttpRequestAsync(method, url, h.Begin(), requester, OnHttpLoad, OnHttpError, request_data, request_data_length);
            assert(top == lua_gettop(L));
            return 0;
        } else {
            assert(top == lua_gettop(L));
            return luaL_error(L, "http.request is not available from this script-type.");
        }
        return 0;
    }

    static const luaL_reg HTTP_COMP_FUNCTIONS[] =
    {
        {"request", Http_Request},
        {0, 0}
    };

    void InitializeHttp(lua_State* L, dmConfigFile::HConfig config_file)
    {
        int top = lua_gettop(L);

        dmScript::RegisterDDFDecoder(dmHttpDDF::HttpResponse::m_DDFDescriptor, &HttpResponseDecoder);

        if (config_file) {
            float timeout = dmConfigFile::GetFloat(config_file, "network.http_timeout", 0.0f);
            g_Timeout = (uint64_t) (timeout * 1000000.0f);
        }

        luaL_register(L, "http", HTTP_COMP_FUNCTIONS);
        lua_pop(L, 1);
        assert(top == lua_gettop(L));
    }

    void FinalizeHttp(lua_State* L)
    {
    }
}
