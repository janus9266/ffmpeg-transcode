# ffmpeg-transcode

### Original Command

ffmpeg.exe -hide_banner -y -i qt.mov -vf yadif -codec:v libx264 -f mp4 -pix_fmt yuv420p -level:v 4.1 -profile:v high qt.mp4
