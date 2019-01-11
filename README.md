# nulla
[Elliptics](https://github.com/bioothod/elliptics) streaming service which supports
adaptive MPEG-DASH and HLS streaming as well as stream muxing.

To implement this we create the whole mpeg container (mp4 or mpeg2ts) in runtime and only read samples data
from audio/video files stored in Elliptics. You can mux multiple audio/video streams (like 5 seconds of this video,
10 seconds of audio from this track and so on) from different tracks stored in Elliptics in runtime.

Video/audio files should be uploaded into Elliptics distributed storage and Nulla service will automatically repack
(without transcoding) them into mp4/mpeg2ts formats suitable for either DASH or HLS streaming.
No need to upload multiple files or create multiple file edits to implement muxing.

To allow muxing all files in the stream must be encoded the same way
(to mux multiple streams codecs must be the same, level/profile info can change).

Here is 5-seconds muxing (5 seconds of the first video, then 5 second of the second,
then next 5 seconds from the first and so on) example, control json will look like this:
```javascript
"tracks": [
{
  "bucket": "b1",
  "key": "video_1.mp4",
  "duration": 5000
},
{
  "bucket": "b1",
  "key": "video_2.mp4",
  "duration": 5000
},
{
  "bucket": "b1",
  "key": "video_1.mp4",
  "start": 5000,
  "duration": 5000
},
{
  "bucket": "b1",
  "key": "video_2.mp4",
  "start": 5000,
  "duration": 5000
}
]
```

Here it is, muxing 2 video and 2 sound channels in the way described above without interruption and gaps.
All 4 files are stored in Elliptics storage as usual objects.

You can check the source of the index.html page to see how muxing is being set up,
you can play with different settings and watch the whole files or mix them in other ways around.
