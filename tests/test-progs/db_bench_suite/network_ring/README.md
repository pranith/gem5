Network ring (MPSC) benchmark
=============================

Models a multi-producer, single-consumer ring used by network stacks. Producers
publish descriptors with release semantics, and the consumer loads them with
acquire semantics while advancing the head pointer.

Examples
--------
./network_ring_bench 2 200000 4096
