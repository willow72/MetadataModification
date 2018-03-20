  "Metadata modification" is a prototype that attempts to solve the problem that all data
is rewritten even when some data is modified in general user application and 
EXT4 file system.
  This project is includes a Linux kernel source code(4.8.15) with a new system call function and
a user application that users the function. It also include a shell script that flushes the
memory buffer. (*Note that there is still an unresolved problem with memory, and after 
the function call you have to free th memory buffer.)

PROJECT : This project is part of a medium-sized research project
"Development of a file system for easy file modification based on variable block"
conducted by Hallym University in Chuncheon, Korea.

License : Copyright (C) 2017, YoungJun Yoo
This project uses a Linux kernel source code and follows th GPL.
Details about the GPL are specified in the "kernel_source/COPYING" file.

Contribution : This source code is a prototype and requires much development.
Therefore, we welcom the participation of many programmers and hackers.
The new system call is included in "kernel_source/kernel/vlfs.c", see
this source (we will soon upload a document containing the concept of
"Metadata modification" and source code content).

