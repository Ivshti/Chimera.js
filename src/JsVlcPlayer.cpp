#include "JsVlcPlayer.h"

v8::Persistent<v8::Function> JsVlcPlayer::_jsConstructor;

struct JsVlcPlayer::AsyncData
{
    virtual void process( JsVlcPlayer* ) = 0;
};

struct JsVlcPlayer::FrameSetupData : public JsVlcPlayer::AsyncData
{
    FrameSetupData( unsigned width, unsigned height ) :
        width( width ), height( height ) {}

    void process( JsVlcPlayer* ) override;

    unsigned width;
    unsigned height;
};

void JsVlcPlayer::FrameSetupData::process( JsVlcPlayer* player )
{
    player->setupBuffer( width, height );
}

struct JsVlcPlayer::FrameUpdated : public JsVlcPlayer::AsyncData
{
    void process( JsVlcPlayer* ) override;
};

void JsVlcPlayer::FrameUpdated::process( JsVlcPlayer* player )
{
    player->frameUpdated();
}

struct JsVlcPlayer::CallbackData : public JsVlcPlayer::AsyncData
{
    CallbackData( JsVlcPlayer::Callbacks_e callback ) :
        callback( callback ) {}

    void process( JsVlcPlayer* );

    JsVlcPlayer::Callbacks_e callback;
};

void JsVlcPlayer::CallbackData::process( JsVlcPlayer* player )
{
    using namespace v8;

    if( player->_jsCallbacks[this->callback].IsEmpty() )
        return;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    Local<Function> callback =
        Local<Function>::New( isolate, player->_jsCallbacks[this->callback] );

    callback->Call( isolate->GetCurrentContext()->Global(), 0, nullptr );
}

JsVlcPlayer::JsVlcPlayer( const v8::Local<v8::Function>& renderCallback ) :
    _libvlc( nullptr ), _jsRawFrameBuffer( nullptr )
{
    _jsCallbacks[CB_FRAME_READY].Reset( v8::Isolate::GetCurrent(), renderCallback );

    _libvlc = libvlc_new( 0, nullptr );
    assert( _libvlc );
    if( _player.open( _libvlc ) ) {
        vlc::basic_vmem_wrapper::open( &_player.basic_player() );
    } else {
        assert( false );
    }

    uv_loop_t* loop = uv_default_loop();

    _async.data = this;
    uv_async_init( loop, &_async,
        [] ( uv_async_t* handle ) {
            if( handle->data )
                reinterpret_cast<JsVlcPlayer*>( handle->data )->handleAsync();
        }
    );
}

JsVlcPlayer::~JsVlcPlayer()
{
    vlc::basic_vmem_wrapper::close();

    _async.data = nullptr;
    uv_close( reinterpret_cast<uv_handle_t*>( &_async ), 0 );
}

unsigned JsVlcPlayer::video_format_cb( char* chroma,
                                       unsigned* width, unsigned* height,
                                       unsigned* pitches, unsigned* lines )
{
    memcpy( chroma, vlc::DEF_CHROMA, sizeof( vlc::DEF_CHROMA ) - 1 );
    *pitches = *width * vlc::DEF_PIXEL_BYTES;
    *lines = *height;

    _tmpFrameBuffer.resize( *pitches * *lines );

    _asyncData.push_back( std::make_shared<FrameSetupData>( *width, *height ) );
    uv_async_send( &_async );

    return 1;
}

void JsVlcPlayer::video_cleanup_cb()
{
    if( !_tmpFrameBuffer.empty() )
        _tmpFrameBuffer.swap( std::vector<char>() );

    _asyncData.push_back( std::make_shared<CallbackData>( CB_FRAME_CLEANUP ) );
    uv_async_send( &_async );
}

void* JsVlcPlayer::video_lock_cb( void** planes )
{
    if( _tmpFrameBuffer.empty() ) {
        *planes = _jsRawFrameBuffer;
    } else {
        if( _jsRawFrameBuffer ) {
            _tmpFrameBuffer.swap( std::vector<char>() );
            *planes = _jsRawFrameBuffer;
        } else {
            *planes = _tmpFrameBuffer.data();
        }
    }

    return nullptr;
}

void JsVlcPlayer::video_unlock_cb( void* /*picture*/, void *const * /*planes*/ )
{

}

void JsVlcPlayer::video_display_cb( void* /*picture*/ )
{
    _asyncData.push_back( std::make_shared<FrameUpdated>() );
    uv_async_send( &_async );
}

void JsVlcPlayer::handleAsync()
{
    while( !_asyncData.empty() ) {
        std::deque<std::shared_ptr<AsyncData> > tmpData;
        _asyncData.swap( _asyncData );
        for( const auto& i: tmpData ) {
            i->process( this );
        }
    }
}

void JsVlcPlayer::setupBuffer( unsigned width, unsigned height )
{
    using namespace v8;

    if( 0 == width || 0 == height )
        return;

    const unsigned frameBufferSize = width * height * vlc::DEF_PIXEL_BYTES;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    Local<Object> global = isolate->GetCurrentContext()->Global();

    Local<Value> abv =
        global->Get(
            String::NewFromUtf8( isolate,
                                 "Uint8Array",
                                 v8::String::kInternalizedString ) );
    Local<Value> argv[] =
        { Integer::NewFromUnsigned( isolate, frameBufferSize ) };
    Local<Object> array =
        Handle<Function>::Cast( abv )->NewInstance( 1, argv );

    array->Set( String::NewFromUtf8( isolate, "width" ),
                Integer::New( isolate, width ) );
    array->Set( String::NewFromUtf8( isolate, "height" ),
                Integer::New( isolate, height) );

    _jsFrameBuffer.Reset( isolate, array );

    _jsRawFrameBuffer =
        static_cast<char*>( array->GetIndexedPropertiesExternalArrayData() );
}

void JsVlcPlayer::frameUpdated()
{
    using namespace v8;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    Local<Function> renderCallback =
        Local<Function>::New( isolate, _jsCallbacks[CB_FRAME_READY] );

    Local<Value> argv[] =
        { Local<Object>::New( isolate, _jsFrameBuffer ) };

    renderCallback->Call( isolate->GetCurrentContext()->Global(),
                          sizeof( argv ) / sizeof( argv[0] ), argv );
}

void JsVlcPlayer::initJsApi()
{
    using namespace v8;

    Isolate* isolate = Isolate::GetCurrent();

    Local<FunctionTemplate> ct = FunctionTemplate::New( isolate, jsCreate );
    ct->SetClassName( String::NewFromUtf8( isolate, "VlcPlayer" ) );
    ct->InstanceTemplate()->SetInternalFieldCount( 1 );

    NODE_SET_PROTOTYPE_METHOD( ct, "play", jsPlay );
    NODE_SET_PROTOTYPE_METHOD( ct, "stop", jsStop );

    _jsConstructor.Reset( isolate, ct->GetFunction() );
}

void JsVlcPlayer::jsCreate( const v8::FunctionCallbackInfo<v8::Value>& args )
{
    using namespace v8;

    if( args.Length() != 1 )
        return;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    Local<Function> renderCallback = Local<Function>::Cast( args[0] );

    if( args.IsConstructCall() ) {
        JsVlcPlayer* jsPlayer = new JsVlcPlayer( renderCallback );
        jsPlayer->Wrap( args.This() );
        args.GetReturnValue().Set( args.This() );
    } else {
        Local<Value> argv[] = { renderCallback };
        Local<Function> constructor =
            Local<Function>::New( isolate, _jsConstructor );
        args.GetReturnValue().Set(
            constructor->NewInstance( sizeof( argv ) / sizeof( argv[0] ),
                                      argv ) );
    }
}

void JsVlcPlayer::jsPlay( const v8::FunctionCallbackInfo<v8::Value>& args )
{
    using namespace v8;

    if( args.Length() != 1 )
        return;

    JsVlcPlayer* jsPlayer = ObjectWrap::Unwrap<JsVlcPlayer>( args.Holder() );
    vlc::player& player = jsPlayer->_player;

    String::Utf8Value mrl( args[0]->ToString() );
    if( mrl.length() ) {
        player.clear_items();
        const int idx = player.add_media( *mrl );
        if( idx >= 0 ) {
            player.play( idx );
        }
    }
}

void JsVlcPlayer::jsStop( const v8::FunctionCallbackInfo<v8::Value>& args )
{
    if( args.Length() != 0 )
        return;

    JsVlcPlayer* jsPlayer = ObjectWrap::Unwrap<JsVlcPlayer>( args.Holder() );
    vlc::player& player = jsPlayer->_player;

    player.stop();
}
