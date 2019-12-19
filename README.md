# tplayer
tiny player using ffmpeg, SDL2

### 说明
一个简单的基于ffmpeg和SDL2的播放器，初衷在于学习ffmpeg和SDL2，主要参考和学习了Martin Bohme的[《An ffmpeg and SDL Tutorial》](http://dranger.com/ffmpeg/tutorial01.html)，以及rambodrahmani的[ffmpeg-video-player](https://github.com/rambodrahmani/ffmpeg-video-player)，以及网上的其他优秀音视频资料。该版本基于最新的ffmpeg(4.2.1)和SDL2(2.0.10) 。

### 编译
编译前需要安装ffmpeg和SDL2：

[ffmpeg下载](https://www.ffmpeg.org/download.html)

[SDL2下载](https://www.libsdl.org/download-2.0.php)

代码无其他依赖，编译只需要运行：sh build.sh，结束后生成tplayer执行程序。

### 运行
tplayer [音频或者视频文件]

### 主要参考资料
[《An ffmpeg and SDL Tutorial》](http://dranger.com/ffmpeg/tutorial01.html)

[ffmpeg-video-player](https://github.com/rambodrahmani/ffmpeg-video-player)

### 其他资料
[FFmpeg+SDL2实现音频流播放](https://juejin.im/post/5cc2af6d5188252e3c71117b)

[FFmpeg学习3：播放音频](https://www.cnblogs.com/wangguchangqing/p/5788805.html)

[FFmpeg例子：resampling_audio分析](https://www.jianshu.com/p/73d706f650e2)

### 写在最后
查找音视频资料的时候，发现了雷霄骅的故事(当年只是略微知晓），颇受感动，在此贴出他的[博客](https://me.csdn.net/leixiaohua1020)，以此纪念。