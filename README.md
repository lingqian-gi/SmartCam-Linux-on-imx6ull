# SmartCam-Linux-on-imx6ull
基于野火iMX6ULL Pro开发板，开发一个带触摸屏GUI的完整嵌入式Linux相机系统，支持：      USB摄像头图像采集（V4L2 + UVC驱动）     MJPEG/YUV双格式处理（硬件MJPEG直出，零CPU开销）     7寸触摸屏本地预览与交互（Qt GUI）     流媒体传输（MJPEG-over-HTTP + RTSP）     私有控制协议（TCP指令控制）     多线程架构（采集→处理→显示→传输）     Linux系统级集成（设备树、systemd服务、开机自启）
