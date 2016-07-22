# dist-conv
Pure Data externals that implement a distributed convolution system

A real-time convolver based on Pure Data. Enables the streaming of impulse
responses and parameter data from a host computer to multiple Raspberry Pi
single board computers. Includes Pure Data externals conv~, bcast~ and
bcreceive~, as well as example PD patches.

---------------
1. Installation
---------------

The host computer only requires the external bcast~. The Raspberry Pi requires
bcreceive~ and conv~.

Linux:

- Run the makefiles in the directories by typing 'make pd_linux'
- Move the .pd_linux files into folder /usr/lib/puredata/extra

TODO: Describe the installation process

--------
2. Usage
--------

TODO: Write usage instructions

----------
3. History
----------

TODO:

- Fix bypass bug
- Expand multithreading to more than one child thread
- Figure out more efficient multithreading
- more bugs are likely

----------
4. Authors
----------

Jussi Nieminen

~bcast and ~bcreceive based on open-source externals ~netsend and ~netreceive
written by Olivier Guillerminet and Olaf Matthes.

----------
5. License
----------

GPLv3. See LICENCE
