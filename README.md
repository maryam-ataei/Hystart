# Improving Hystart


Hystart is designed to exit before the loss to avoid overshooting link throughput. It exits slow start at the safe point to avoid overshooting.
Hystart may exit slow start prematurely and it is problematic for links with high bandwidth and high latency, such as a satellite internet connection.

Hystart relies on 2 exit triggers:
*Delay detect: If delay increases above the threshold assume queuing and enter congestion avoidance.
*Ack Packet train: try to estimate the bandwidth of the link and exit slow start when near that value.

To improve Hystart performance, we try to improve Hystart's delay detection. Some statistics about the link need to be monitored (Average RTT (Round trip time) and Standard deviation (or variance)) and based on them adjust the exit threshold.

To calculate standard deviation, we use Welford’s algorithm. Therefore, Maintains 3 values:
* Count (number of data points computed)
* Mean (Running average value of all points seen thus far)
* ![First equation](https://latex.codecogs.com/svg.image?M_%7B2%7D) the aggregatedifference from the mean: <br/> ![Second equation](https://latex.codecogs.com/svg.image?M_%7B2%7D=%5Csum_%7Bn%7D%5E%7Bi=1%7D(x_%7Bi%7D-%5Cbar%7Bx_%7Bn%7D%7D)%5E%7B2%7D)
* Variance is ![Third equation](https://latex.codecogs.com/svg.image?%5Cfrac%7BM_%7B2%7D%7D%7Bn%7D) and thus standard deviation is the square root


By changing the following files, the required field is added to socket structure, stats are updated on each received ACK, and can be read statistics to check if HyStart should exit.
* include/linux/tcp.h
* net/ipv4/tcp.c
* net/ipv4/tcp_input.c
* net/ipv4/tcp_cubic.c
