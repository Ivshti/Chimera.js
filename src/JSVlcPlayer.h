#pragma once

#include <memory>
#include <deque>

#include <v8.h>
#include <node.h>
#include <node_object_wrap.h>
#include <uv.h>

#include <libvlc_wrapper/vlc_player.h>
#include <libvlc_wrapper/vlc_vmem.h>

class JsVlcPlayer :
    public node::ObjectWrap, private vlc::basic_vmem_wrapper
{
public:
    static void initJsApi();
    static void jsCreate( const v8::FunctionCallbackInfo<v8::Value>& args );

private:
    static void jsPlay( const v8::FunctionCallbackInfo<v8::Value>& args );
    static void jsStop( const v8::FunctionCallbackInfo<v8::Value>& args );

private:
    JsVlcPlayer( const v8::Local<v8::Function>& renderCallback );
    ~JsVlcPlayer();

    enum Callbacks_e {
        CB_FRAME_SETUP,
        CB_FRAME_READY,
        CB_FRAME_CLEANUP,

        CB_MAX,
    };

    struct AsyncData;
    struct FrameSetupData;
    struct FrameUpdated;
    struct CallbackData;

    void handleAsync();
    void setupBuffer( unsigned width, unsigned height );
    void frameUpdated();

private:
    unsigned video_format_cb( char* chroma,
                              unsigned* width, unsigned* height,
                              unsigned* pitches, unsigned* lines ) override;
    void video_cleanup_cb() override;

    void* video_lock_cb( void** planes ) override;
    void video_unlock_cb( void* picture, void *const * planes ) override;
    void video_display_cb( void* picture ) override;

private:
    static v8::Persistent<v8::Function> _jsConstructor;

    libvlc_instance_t* _libvlc;
    vlc::player _player;

    uv_async_t _async;
    std::deque<std::shared_ptr<AsyncData> > _asyncData;

    std::vector<char> _tmpFrameBuffer;
    v8::Persistent<v8::Object> _jsFrameBuffer;
    char* _jsRawFrameBuffer;

    v8::Persistent<v8::Function> _jsCallbacks[CB_MAX];
};
