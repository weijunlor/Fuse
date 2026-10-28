RTPS packet contents, Cyclone default QoS vs per-topic QoS (#14), 2026-10-28.
Captured with: sudo timeout 3 tcpdump -U -i any -w <file> udp
while build/server and one build/dds_joiner ran (FUSE_QOS=default for the first file).
Decoded per packet (RTPS header + submessages with their sizes):

fuse_qos_default.pcap (180 RTPS packets in 3 s)
   179 x  224 B: header 20, INFO_TS 12, DATA 160, HEARTBEAT 32
     1 x  360 B: header 20, INFO_DST 16, INFO_TS 12, DATA 312
fuse_qos_tuned.pcap (179 RTPS packets in 3 s)
   178 x  192 B: header 20, INFO_TS 12, DATA 160
     1 x  360 B: header 20, INFO_DST 16, INFO_TS 12, DATA 312

The 224 B packets are GameSnapshots (DATA 160 = 132-byte sample + encapsulation
and submessage fields). With a reliable writer (Cyclone's default) each one
carries a 32-byte HEARTBEAT ("I have sent up to sequence number N; tell me what
you are missing"), even though no reader is reliable. With the best-effort
writer of #14 it is gone. Open the .pcap files in Wireshark (RTPS dissector)
to see the same.
