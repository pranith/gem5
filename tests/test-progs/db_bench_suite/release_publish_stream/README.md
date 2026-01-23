Release publish stream benchmark
================================

Models a producer payload stream where each producer writes a dense, contiguous
payload (merge-buffer friendly stores) before publishing a release store that
makes the data visible to a consumer.

Examples
--------
./release_publish_stream_bench 4 200000 4096 256 1048576
