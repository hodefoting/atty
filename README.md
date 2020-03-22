atty - audio sound device for terminals
=======================================

A protocol extending ECMA-48 based terminals like xterm, linux console,
gnome-terminal and more with audio playback and capture.

Atty provides in a single binary commandline tools to configure and simplify
use of a complying terminal; this binary also provides a wrapper implementation
that you can run in any linux terminal to bridge the escape sequences to
audio hardware abstracted by SDL, it might possible even work on OSX.

Chunks of audio are transmitted in base64 or ascii85 encoding as APC escape
sequences. With the default settings this brings back the sunaudio device for
use in terminal applications

Some example commandlines:

$ atty

or

$ atty status

print, current audio configuration - and make a test sound,
possibly enable audio for shell

$ atty samplerate=48000 bits=16 type=signed

change audio parameters. or if 

$ atty mic > recording

opens up a recording session, abort with ctrl-c, raw samples in the
configured format appears in the file recording.

$ cat recording | atty speaker
or 
$ atty speaker < recording

Will play back the same file.


Protocol
--------

The protocol is formed around an APC message ESC _ A the protocol is
inspired by how kitty does its image transport:

[ESC]_Ak=v,k=v,k=v;payload[ESC]\

This message format is used both for audio/configuration sent from the host
(your program) to the terminal, and for responses/microphone input.

Sending [ESC]_Aa=q;[ESC]\ sets the action key to q - for query. This queries
the reply with the default atty settings is:

[ESC]_As=8000,b=8,c=1,T=u,e=a,o=0;OK[ESC]\

Breaking down these key/value pairs we get:

s=8000   samplerate in hz
b=8      bits per sample, 8 and 16 are valid
c=1      mono/interleaved stereo 1/2
T=u      sample type, u = ulaw    s = signed
e=a      encoding     a = ascii85 b = base64
o=0      compression  z = deflate(zlib) o = opus  0 = none

To change the settings to 48000hz, 16bit stereo the following would be issued,
only z-lib compression is supported at the moment.

[ESC]_As=48000,b=16,c=2,T=s;[ESC]\

You might have to wait for a few, perhaps 4 seconds of silence to be able to
change the audio settings. At the start of your program, there is no such
issue. You should query the terminal for the actual audio settings and check
that they match your expectations after setting them.

The audio packet payload is encoded as either base64 or ascii85 (more
efficient) raw data, or optionally compressed with zlib.

The recognized values for a key can be queried with:

[ESC]_As=?;[ESC]\

This generates responses of the form:

[ESC]_As=?;8000,16000,24000,480000[ESC]\

At the moment all valid keys are expressed as comma separated lists,

Audio can also flow in the other direction, if the terminal supports (and
possibly - up to the terminal implementation if user acknowledges a microphone
request.)

Recording uses the same settings as playback, passing the value 1 to
the key 'm' turns on recording, and 0 turns it off.

Possible future plans
---------------------

Prompt operator to acknowledge use of microphone (blink terminal, and show message in titlebar and hijack keypresses until either y or n.

Add support for using opus as codec in addition to zlib for compressing the payload.

Split configuration of microphone/speaker?

To support the development of atty and dissimilar technologies; consider
supporting the author at https://patreon.com/pippin and
https://liberapay.com/pippin

