#include <unistd.h>
#include <sys/stat.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>
#include <libswresample/swresample.h>
#include "tool.h"

#define V_QUEUE_MAX_SIZE 100          //视频的包队列最大长度
#define A_QUEUE_MAX_SIZE 200          //音频的包队列最大长度
#define AUDIO_SAMPLE_BUFFER_SIZE 2048 //音频设备内部缓存区的大小（单位为采样数）
#define WAIT_TIME_MS 5                //轮询等待时间
#define SAMPLE_BYTE 2                 //采样编码对应的字节数
#define DISPLAY_WIDTH 800             //播放窗口最大宽度
#define DISPLAY_HEIGHT 600            //播放窗口最大高度

#define FF_REFRESH_EVENT (SDL_USEREVENT)  //播放刷新事件
#define FF_QUIT_EVENT (SDL_USEREVENT + 1) //退出事件

//GlobalContext表示全局上下文，可以对应为类
typedef struct
{
    AVFormatContext *formatCtx;
    AVCodecContext *vCodecCtx;
    AVCodecContext *aCodecCtx;
    AVCodec *vCodec;
    AVCodec *aCodec;
    AVStream *vStream;
    AVStream *aStream;
    SafeQueue *vPktQueue; //视频包队列
    SafeQueue *aPktQueue; //音频包队列
    int vStreamIndex;
    int aStreamIndex;

    struct SwsContext *sws; //视频帧输出转换器

    SDL_AudioDeviceID aDeviceID; //音频输出设备id
    SwrContext *swr;             //音频帧输出转换器
    uint8_t *aBuffer;            //音频帧输出缓存

    //SDL播放用
    SDL_Window *screen;
    SDL_Renderer *renderer;
    SDL_Texture *texture;

    //退出标志
    int quit;

    //当前显示帧
    AVFrame *dispFrame;
    pthread_mutex_t mutex; //当前显示帧的锁定（多线程锁）

    //当前音频的时钟（同步视频数据用）
    double audioClock;

} GlobalContext;

//初始化解码器和context
static int openCodec(char *typeName, AVStream *stream, AVCodec **codec, AVCodecContext **codecCtx)
{
    *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (*codec == NULL)
    {
        printf("(%s) no support codec(%d)!\n", typeName, stream->codecpar->codec_id);
        return -1;
    }

    //解码器初始化
    *codecCtx = avcodec_alloc_context3(*codec);
    if (*codecCtx == NULL)
    {
        printf("(%s) avcodec_alloc_context3 failed!\n", typeName);
        return -1;
    }
    //赋值参数
    if (avcodec_parameters_to_context(*codecCtx, stream->codecpar) < 0)
    {
        printf("(%s) avcodec_parameters_to_context failed!\n", typeName);
        return -1;
    }
    // Open codec
    if (avcodec_open2(*codecCtx, *codec, NULL) < 0)
    {
        printf("(%s) avcodec_open2 failed!\n", typeName);
        return -1;
    }
    return 0;
}

//计算播放器的宽高
void computeDisplay(int vWidth, int vHeight, int *width, int *height)
{
    int computeWidth = DISPLAY_WIDTH;
    if (vWidth < computeWidth)
    {
        computeWidth = vWidth;
    }
    int computeHeight = vHeight * computeWidth / vWidth;
    *width = computeWidth;
    *height = computeHeight;
    printf("[disp] orig_width: %d, orig_height: %d, disp_width: %d, disp_height: %d\n", vWidth, vHeight, *width, *height);
}

//初始化全局上下文
static GlobalContext *initContext(char *infile)
{
    GlobalContext *gc = (GlobalContext *)malloc(sizeof(GlobalContext));

    gc->quit = 0;
    gc->dispFrame = NULL;
    gc->audioClock = 0;
    int ret = pthread_mutex_init(&gc->mutex, NULL);
    if (ret != 0)
    {
        printf("mutex error(%d)!\n", ret);
        return NULL;
    }

    gc->formatCtx = NULL;

    if (avformat_open_input(&gc->formatCtx, infile, NULL, NULL) != 0)
    {
        printf("avformat_open_input failed!\n");
        return NULL;
    }

    if (avformat_find_stream_info(gc->formatCtx, NULL) < 0)
    {
        printf("avformat_find_stream_info failed!\n");
        return NULL;
    }

    gc->vStreamIndex = -1;
    gc->aStreamIndex = -1;

    for (int i = 0; i < gc->formatCtx->nb_streams; i++)
    {
        if (gc->formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            gc->vStream = gc->formatCtx->streams[i];
            gc->vStreamIndex = i;
        }
        else if (gc->formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            gc->aStream = gc->formatCtx->streams[i];
            gc->aStreamIndex = i;
        }
    }

    // must have audio stream
    if (gc->aStreamIndex == -1)
    {
        printf("[error] no found audio stream!\n");
        return NULL;
    }

    int err;

    // video initialize
    if (gc->vStreamIndex != -1)
    {
        err = openCodec("video", gc->vStream, &gc->vCodec, &gc->vCodecCtx);
        if (err != 0)
        {
            return NULL;
        }
        //set display width and height
        int width;
        int height;
        computeDisplay(gc->vCodecCtx->width, gc->vCodecCtx->height, &width, &height);

        gc->screen = SDL_CreateWindow(
            "tplayer",
            SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED,
            width,
            height,
            0);

        if (!gc->screen)
        {
            printf("create screen failed!\n");
            return NULL;
        }

        gc->renderer = SDL_CreateRenderer(gc->screen, -1, 0);
        if (!gc->renderer)
        {
            printf("create render failed!\n");
            return NULL;
        }

        // Allocate a place to put our YUV image on that screen
        gc->texture = SDL_CreateTexture(
            gc->renderer,
            SDL_PIXELFORMAT_IYUV,
            SDL_TEXTUREACCESS_STREAMING,
            gc->vCodecCtx->width,
            gc->vCodecCtx->height);

        if (!gc->texture)
        {
            printf("create texture failed!\n");
            return NULL;
        }

        // initialize SWS context for software scaling
        gc->sws = sws_getContext(gc->vCodecCtx->width,
                                 gc->vCodecCtx->height,
                                 gc->vCodecCtx->pix_fmt,
                                 gc->vCodecCtx->width,
                                 gc->vCodecCtx->height,
                                 AV_PIX_FMT_YUV420P,
                                 SWS_BILINEAR,
                                 NULL,
                                 NULL,
                                 NULL);

        gc->vPktQueue = mkqueue(V_QUEUE_MAX_SIZE);
    }

    //audio initialize
    if (gc->aStreamIndex != -1)
    {
        err = openCodec("audio", gc->aStream, &gc->aCodec, &gc->aCodecCtx);
        if (err != 0)
        {
            return NULL;
        }
        // audio specs containers
        SDL_AudioSpec wanted_specs;
        SDL_AudioSpec specs;

        wanted_specs.freq = gc->aCodecCtx->sample_rate;
        wanted_specs.format = AUDIO_S16SYS; //!!!应该是AUDIO开头的变量，而不是AV开头的变量
        wanted_specs.channels = gc->aCodecCtx->channels;
        wanted_specs.silence = 0;
        wanted_specs.samples = AUDIO_SAMPLE_BUFFER_SIZE; //frame buffer size
        wanted_specs.callback = NULL;
        wanted_specs.userdata = NULL;

        // open audio device
        gc->aDeviceID = SDL_OpenAudioDevice(
            NULL,
            0,
            &wanted_specs,
            &specs,
            SDL_AUDIO_ALLOW_FORMAT_CHANGE);

        if (gc->aDeviceID == 0)
        {
            printf("failed to open audio device: %s\n", SDL_GetError());
            return NULL;
        }

        SDL_PauseAudioDevice(gc->aDeviceID, 0);

        gc->swr = swr_alloc_set_opts(NULL,                          // we're allocating a new context
                                     gc->aCodecCtx->channel_layout, // out_ch_layout
                                     AV_SAMPLE_FMT_S16,             // out_sample_fmt
                                     gc->aCodecCtx->sample_rate,    // out_sample_rate
                                     gc->aCodecCtx->channel_layout, // in_ch_layout
                                     gc->aCodecCtx->sample_fmt,     // in_sample_fmt
                                     gc->aCodecCtx->sample_rate,    // in_sample_rate
                                     0,                             // log_offset
                                     NULL);
        swr_init(gc->swr);

        int maxBufferSize = av_samples_get_buffer_size(NULL, gc->aCodecCtx->channels, AUDIO_SAMPLE_BUFFER_SIZE, AV_SAMPLE_FMT_S16, 1);
        gc->aBuffer = (uint8_t *)av_malloc(maxBufferSize);

        gc->aPktQueue = mkqueue(A_QUEUE_MAX_SIZE);
    }

    return gc;
}

//清理全局context
static void freeContext(GlobalContext *gc)
{
    SDL_Quit();
    if (gc->vStreamIndex != -1)
    {
        avcodec_close(gc->vCodecCtx);

        SDL_DestroyWindow(gc->screen);
        SDL_DestroyRenderer(gc->renderer);
        SDL_DestroyTexture(gc->texture);

        av_frame_free(&gc->dispFrame);

        swr_free(&gc->swr);
    }

    if (gc->aStreamIndex != -1)
    {
        avcodec_close(gc->aCodecCtx);
        free(gc->aBuffer);

        // sws_freeContext(gc->sws); //此方法调用在强制退出时候有异常，暂不知晓原因，先去掉；
    }

    avformat_close_input(&gc->formatCtx);
}

//发送结束事件（异常错误退出时，音频播放结束时，或者手动关闭，或者键盘ESC退出时调用）
static void sendQuitEvent()
{
    SDL_Event event;
    event.type = FF_QUIT_EVENT;
    SDL_PushEvent(&event);
}

static Uint32 refreshCallback(Uint32 interval, void *param)
{
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;
    SDL_PushEvent(&event);
    return 0;
}

//设置视频帧的刷新
static void delayRefresh(int delay)
{
    // schedule an SDL timer
    int ret = SDL_AddTimer(delay, refreshCallback, NULL);

    if (ret == 0)
    {
        printf("Could not schedule refresh callback: %s.\n.", SDL_GetError());
    }
}

//获取当前的音频帧时刻（用于视频帧的同步）
static double getCurAudioClock(GlobalContext *gc)
{
    int sampleBytesPerSec = gc->aCodecCtx->channels * SAMPLE_BYTE * gc->aCodecCtx->sample_rate; //每秒采样大小
    int leftBytes = SDL_GetQueuedAudioSize(gc->aDeviceID); //音频设备缓冲区未播放的大小
    double leftSecs = 1.0 * leftBytes / sampleBytesPerSec;
    double clock = gc->audioClock - leftSecs;
    return clock;
}

//视频帧的延迟计算
static int getVideoRefreshDelay(GlobalContext *gc, AVFrame *frame)
{
    double audioClock = getCurAudioClock(gc);
    double videoClock = frame->pts * av_q2d(gc->vStream->time_base); //视频帧的时刻
    double diff = videoClock - audioClock;
    int delay = 0;
    if (diff > 0)
    {
        delay = (int)(1000 * diff);
    }
    if (delay == 0)
    {
        delay = 1;
    }
    return delay;
}

//设置当前视频帧：从视频队列中读出视频帧，并进行转换，然后设置延迟刷新
static int videoPrepareThread(void *data)
{
    GlobalContext *gc = (GlobalContext *)data;
    AVFrame *frame = av_frame_alloc();
    while (1)
    {

        if (gc->quit)
        {
            break;
        }

        //设置当前显示帧
        pthread_mutex_lock(&gc->mutex);
        if (gc->dispFrame != NULL)
        { //当前帧已经存在，但没有显示，则继续等待
            pthread_mutex_unlock(&gc->mutex);
            SDL_Delay(WAIT_TIME_MS);
            continue;
        }

        AVPacket *pkt = (AVPacket *)dequeue(gc->vPktQueue);
        if (pkt == NULL)
        {
            pthread_mutex_unlock(&gc->mutex);
            SDL_Delay(WAIT_TIME_MS);
            continue;
        }

        //处理当前帧
        int r = avcodec_send_packet(gc->vCodecCtx, pkt);
        if (r < 0)
        {
            printf("video avcodec_send_packet error!\n");
            sendQuitEvent();
            return -1;
        }

        int ret = avcodec_receive_frame(gc->vCodecCtx, frame);
        if (ret == AVERROR(EAGAIN)) //有时需要预读多个视频帧才能有新的视频帧生成（例如：非关键帧）
        {
            pthread_mutex_unlock(&gc->mutex);
            continue;
        }
        else if (ret < 0)
        {
            printf("video avcodec_receive_frame error!\n");
            sendQuitEvent();
            return -1;
        }

        //生成新的视频帧
        gc->dispFrame = av_frame_alloc();
        int numBytes = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, gc->vCodecCtx->width, gc->vCodecCtx->height, 1);

        //在dispFrame中加入buffer空间（av_frame_alloc没有创建空间）
        uint8_t *buffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));
        av_image_fill_arrays(gc->dispFrame->data, gc->dispFrame->linesize, buffer, AV_PIX_FMT_YUV420P, gc->vCodecCtx->width, gc->vCodecCtx->height, 1);

        // Convert the image into YUV format that SDL uses
        sws_scale(gc->sws, (uint8_t const *const *)frame->data,
                  frame->linesize, 0, gc->vCodecCtx->height, gc->dispFrame->data,
                  gc->dispFrame->linesize);

        pthread_mutex_unlock(&gc->mutex);

        //设置refresh delay
        int delayMs = getVideoRefreshDelay(gc, frame);
        delayRefresh(delayMs);

        av_packet_free(&pkt);
    }
    av_frame_free(&frame);
    return 0;
}

//音频播放线程（播放完成后自动退出）
static int audioThread(void *data)
{
    GlobalContext *gc = (GlobalContext *)data;
    AVFrame *frame = av_frame_alloc();

    SDL_Delay(20); //等待包初始填充
    while (1)
    {

        if (gc->quit)
        {
            break;
        }

        AVPacket *pkt = (AVPacket *)dequeue(gc->aPktQueue);
        if (pkt == NULL)
        {
            //检查播放缓存是否有数据剩余
            int leftSize = SDL_GetQueuedAudioSize(gc->aDeviceID);
            if (leftSize > 0)
            {
                SDL_Delay(WAIT_TIME_MS);
                continue;
            }
            else
            {
                //finish!
                break;
            }
        }

        int r = avcodec_send_packet(gc->aCodecCtx, pkt);
        if (r < 0)
        {
            printf("audio avcodec_send_packet error!\n");
            sendQuitEvent();
            return -1;
        }

        while (avcodec_receive_frame(gc->aCodecCtx, frame) >= 0)
        {
            //采样数据转换，AUDIO_SAMPLE_BUFFER_SIZE表示最大的转换输出大小（单位：采样数），outSamples表示真实的输出大小
            //注意：有可能outSamples小于nb_samples，此时swr内部有缓存保存多余的数据，可以通过swr_get_delay查看缓存sample数
            int outSamples = swr_convert(gc->swr, &gc->aBuffer, AUDIO_SAMPLE_BUFFER_SIZE, (const uint8_t **)frame->data, frame->nb_samples);
            if (outSamples < 0)
            {
                printf("swr_convert failed!\n");
                sendQuitEvent();
                return -1;
            }

            // 输出sample对应的buffer大小，SDL_QueueAudio的单位是字节，不是sample数，所以需要获取大小
            int outBufSize = av_samples_get_buffer_size(NULL, gc->aCodecCtx->channels, outSamples, AV_SAMPLE_FMT_S16, 1);
            int ret = SDL_QueueAudio(gc->aDeviceID, gc->aBuffer, outBufSize);
            if (ret < 0)
            {
                printf("SDL_QueueAudio failed(%d)!\n", ret);
                sendQuitEvent();
                return -1;
            }

            //更新audio时钟
            gc->audioClock = frame->pts * av_q2d(gc->aStream->time_base);
        }

        av_packet_free(&pkt);
    }
    av_frame_free(&frame);

    printf("Audio finish!\n");
    gc->quit = 1;
    sendQuitEvent();
    return 0;
}

//从文件中读取包，并且发送给音频，视频队列
static int readPktThead(void *data)
{
    GlobalContext *gc = (GlobalContext *)data;
    // read data
    AVPacket *packet = av_packet_alloc();

    while (1)
    {

        if (gc->aStreamIndex != -1)
        {
            int asize = qsize(gc->aPktQueue);
            if (asize >= A_QUEUE_MAX_SIZE)
            {
                SDL_Delay(WAIT_TIME_MS);
                continue;
            }
        }

        if (gc->vStreamIndex != -1)
        {
            int vsize = qsize(gc->vPktQueue);
            if (vsize >= V_QUEUE_MAX_SIZE)
            {
                SDL_Delay(WAIT_TIME_MS);
                continue;
            }
        }

        int ret = av_read_frame(gc->formatCtx, packet);
        if (ret < 0)
        {
            printf("Read finish!\n");
            break;
        }
        AVPacket *pkt = av_packet_clone(packet);
        av_packet_unref(packet);

        if (pkt->stream_index == gc->vStreamIndex)
        {
            enqueue(gc->vPktQueue, pkt);
        }
        else if (pkt->stream_index == gc->aStreamIndex)
        {
            enqueue(gc->aPktQueue, pkt);
        }
    }
    return 0;
}

//显示当前帧
static void refreshDisplay(GlobalContext *gc)
{
    pthread_mutex_lock(&gc->mutex);
    if (gc->dispFrame == NULL)
    {
        printf("dispFrame is null!\n");
        pthread_mutex_unlock(&gc->mutex);
        return;
    }
    //显示帧
    SDL_UpdateYUVTexture(
        gc->texture,
        NULL,
        gc->dispFrame->data[0],
        gc->dispFrame->linesize[0],
        gc->dispFrame->data[1],
        gc->dispFrame->linesize[1],
        gc->dispFrame->data[2],
        gc->dispFrame->linesize[2]);

    SDL_RenderClear(gc->renderer);
    SDL_RenderCopy(gc->renderer, gc->texture, NULL, NULL);
    SDL_RenderPresent(gc->renderer);

    //清除帧
    av_frame_free(&gc->dispFrame);
    gc->dispFrame = NULL;
    pthread_mutex_unlock(&gc->mutex);
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("Usage: play3 [file]\n");
        exit(1);
    }
    char *infile = argv[1];

    struct stat filebuf;
    int exist = stat(infile, &filebuf);
    if (exist != 0)
    {
        printf("file(%s) not exists!\n", infile);
        exit(1);
    }

    //一个工具，可以快速打印段错误的栈信息，方便排查错误
    if (signal(SIGSEGV, sighandler_dump_stack) == SIG_ERR)
    {
        printf("can't catch SIGSEGV!\n");
        exit(1);
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
    {
        printf("Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }
    GlobalContext *gc = initContext(infile);
    if (gc == NULL)
    {
        printf("init global context failed!\n");
        exit(1);
    }

    SDL_Thread *pktThread = SDL_CreateThread(readPktThead, "read_pkg", gc);
    SDL_Thread *aThread = SDL_CreateThread(audioThread, "audio_thread", gc);
    if (gc->vStreamIndex != -1)
    {
        SDL_Thread *vPrepareThread = SDL_CreateThread(videoPrepareThread, "video_prepare", gc);
    }

    SDL_Event event;
    int ret;
    while (1)
    {
        ret = SDL_WaitEvent(&event);
        if (ret == 0)
        {
            printf("SDL_WaitEvent failed: %s.\n", SDL_GetError());
        }

        switch (event.type)
        {
        case FF_QUIT_EVENT:
        case SDL_QUIT:
            gc->quit = 1;
            break;

        case SDL_KEYDOWN:
            if (event.key.keysym.sym == SDLK_ESCAPE)
            { // ESC键退出
                gc->quit = 1;
            }
            break;

        case FF_REFRESH_EVENT:
            refreshDisplay(gc);
            break;

        default:
            break;
        }

        if (gc->quit)
        {
            printf("Quit!\n");
            break;
        }
    }

    freeContext(gc);
    return 0;
}