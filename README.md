# Wing_apps
**Applications for WING, using wapi or OSC**

This repository regroups a collection of applications aimed at remote-controlling a WING mixing console from Music-Group/Behringer, either using the **wapi** library (see https://github/pmaillot/wapi) or **OSC**. 
Both protocols are fully detailed in the **Wing Remote Protocols document** I write and maintain with the authorization of Music Group, and can be found at https://drive.google.com/file/d/1-iptgd2Uxw4qPEbmegG2Sqccf8AbRRfk/view?usp=drive_link

Compiling wapi-based application is typically done with the following script, assuming you are using gcc for compiler:
```
gcc -O3 -Wall -I<wapi include dir> -o <file>.exe <file>.c -L<wapi lib dir> -lwapi -lws2_32 -lgdi32 -mwindows
```
