# Wing_apps
**Applications for WING, using wapi or OSC**

This repository regroups a collection of applications aimed at remote-controlling a WING mixing console from Music-Group/Behringer, either using the **wapi** library (see https://github/pmaillot/wapi) or **OSC**. 
Both protocols are fully detailed in the **Wing Remote Protocols document** I write and maintain with the authorization of Music Group, and can be found at https://drive.google.com/file/d/1-iptgd2Uxw4qPEbmegG2Sqccf8AbRRfk/view?usp=drive_link

Compiling wapi-based application is typically done with the following script, assuming you are using gcc for compiler:
```
gcc -O3 -Wall -I<wapi include dir> -o <file>.exe <file>.c -L<wapi lib dir> -lwapi -lws2_32 -lgdi32 -mwindows
```

 --------------------------------------------------------------------------------------
 ## Applications:

### wpanel: 

A WING panel interface you can use as a remote surface or control tool for your mixing needs; It provides the standard WING strips in a computer window,  interfacing with a real console over the network (automatically finding the console), and providing pre/post fader meter/dyn/gate feedback as well as Select/Solo/Mute/Fader controls in both directions.
WING banks can be selected, adjusting the application window size to minimize screen space use. DCA and FX controls are not part of the app at this time.

This app has been co-written with the help of ClaudeAI, as an exercise to show AI is fully capable of grasping the Wing Remote Protocols document, and effectively extract methods and API protocol for using wapi, creating a working application without having to own/use a compiler on your PC. The app development spreaded over several days of adjusting and tweaking, ensuring Claude had a correct understanding of what I wanted to achieve. Claude's approach to WIN32 (GUI portions of the application) is different from what I would have coded, but is quite effective and so much faster than me doing it!
I had to help Claude with fully understanding how to address metering, especially as it was originally using an older (and no longer accurate) version of the Wing Remote Protocols document.

--------------------------------------------------------------------------------------
