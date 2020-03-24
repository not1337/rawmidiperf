 rawmidiperf, a test utility for latency and throughput of MIDI interfaces
                        (c) 2020 Andreas Steinmetz

---------------------------------------------------------------------------


  A utility to measure latency and throughput of MIDI interfaces.
===================================================================

This is a loopback test utility for MIDI interfaces attached to Linux systems.
It uses ALSA raw MIDI devices.

This utility is intended for developers and people who want to know
roundtrip latency and throughput details of their MIDI interfaces.

To use this utility one has to connect the MIDI in and MIDI out of
an interface port with a MIDI cable. This needs to be done for all
ports to be tested in parallel.

For further details, compile the utility and run it without any
command line option or see the source code.
