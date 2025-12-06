<p>
  <img src="icon.png" alt="audiomap icon" width="128">
</p>

audiomap is a visual audio explorer.  
it maps files onto a 2d plane based on sonic information, to find similar sounds visually.  
there is also a drag mode toggled with `d` to drag files into other programs easily.  

**filetypes**

`wav`, `flac`, `mp3`, `m4a`, `wma`, `aac`, `ogg`, `aiff` 

**controls**

| input | action |
| :--- | :--- |
| left/right drag | pan the viewport |
| scroll wheel | zoom in and out |
| click dot | play the sample |
| right click dot | view file statistics |
| drag mode | drag files into other windows |

**analysis**

| component | description |
| :--- | :--- |
| x-axis | zero crossing rate (noisiness/timbre) |
| y-axis | root mean square (loudness/energy) |
| color | calculated from zcr density |
| lines | connect audibly similar samples on hover |
| oscilloscope | real-time waveform visualization on playback |

**keys**

| key | action |
| :--- | :--- |
| o | open folder |
| l | toggle list view |
| d | toggle drag mode |
| s | stop playback |
| arrows | pan view |
| pgup/dn | zoom view |
| esc | close list / quit app |
