/*
 * TCP CUBIC: Binary Increase Congestion control for TCP v2.3
 * Home page:
 *      http://netsrv.csc.ncsu.edu/twiki/bin/view/Main/BIC
 * This is from the implementation of CUBIC TCP in
 * Sangtae Ha, Injong Rhee and Lisong Xu,
 *  "CUBIC: A New TCP-Friendly High-Speed TCP Variant"
 *  in ACM SIGOPS Operating System Review, July 2008.
 * Available from:
 *  http://netsrv.csc.ncsu.edu/export/cubic_a_new_tcp_2008.pdf
 *
 * CUBIC integrates a new slow start algorithm, called HyStart.
 * The details of HyStart are presented in
 *  Sangtae Ha and Injong Rhee,
 *  "Taming the Elephants: New TCP Slow Start", NCSU TechReport 2008.
 * Available from:
 *  http://netsrv.csc.ncsu.edu/export/hystart_techreport_2008.pdf
 *
 * All testing results are available from:
 * http://netsrv.csc.ncsu.edu/wiki/index.php/TCP_Testing
 *
 * Unless CUBIC is enabled and congestion window is large
 * this behaves the same as the original Reno.
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/math64.h>
#include <net/tcp.h>

#define BICTCP_BETA_SCALE    1024	/* Scale factor beta calculation
					 * max_cwnd = snd_cwnd * beta
					 */
#define	BICTCP_HZ		10	/* BIC HZ 2^10 = 1024 */

/* Two methods of hybrid slow start */
#define HYSTART_ACK_TRAIN	0x1
#define HYSTART_DELAY		0x2

/* Number of delay samples for detecting the increase of delay */
#define HYSTART_MIN_SAMPLES	8
#define HYSTART_DELAY_MIN	(4U<<3)
/* #define HYSTART_DELAY_MAX	(16U<<3) */
#define HYSTART_DELAY_MAX	(hystart_delay_max ? (16U<<3) : INT_MAX)
#define HYSTART_DELAY_THRESH(x)	clamp(x, HYSTART_DELAY_MIN, HYSTART_DELAY_MAX)

static int fast_convergence __read_mostly = 1;
static int beta __read_mostly = 717;	/* = 717/1024 (BICTCP_BETA_SCALE) */
static int initial_ssthresh __read_mostly;
static int bic_scale __read_mostly = 41;
static int tcp_friendliness __read_mostly = 1;

static int hystart __read_mostly = 1;
static int hystart_detect __read_mostly = HYSTART_ACK_TRAIN | HYSTART_DELAY;
static int hystart_low_window __read_mostly = 16;
static int hystart_ack_delta __read_mostly = 2;
/* This variable is used to switch between clamping
 * HYSTART_DELAY_THRESH between 16ms and UINT_MAX
 */
static int hystart_delay_max __read_mostly = 1;

static u32 cube_rtt_scale __read_mostly;
static u32 beta_scale __read_mostly;
static u64 cube_factor __read_mostly;

//int total_pkts = 0;

/* Note parameters that are used for precomputing scale factors are read-only */
module_param(fast_convergence, int, 0644);
MODULE_PARM_DESC(fast_convergence, "turn on/off fast convergence");
module_param(beta, int, 0644);
MODULE_PARM_DESC(beta, "beta for multiplicative increase");
module_param(initial_ssthresh, int, 0644);
MODULE_PARM_DESC(initial_ssthresh, "initial value of slow start threshold");
module_param(bic_scale, int, 0444);
MODULE_PARM_DESC(bic_scale, "scale (scaled by 1024) value for bic function (bic_scale/1024)");
module_param(tcp_friendliness, int, 0644);
MODULE_PARM_DESC(tcp_friendliness, "turn on/off tcp friendliness");
module_param(hystart, int, 0644);
MODULE_PARM_DESC(hystart, "turn on/off hybrid slow start algorithm");
module_param(hystart_detect, int, 0644);
MODULE_PARM_DESC(hystart_detect, "hybrid slow start detection mechanisms"
		 " 1: packet-train 2: delay 3: both packet-train and delay");
module_param(hystart_low_window, int, 0644);
MODULE_PARM_DESC(hystart_low_window, "lower bound cwnd for hybrid slow start");
module_param(hystart_ack_delta, int, 0644);
MODULE_PARM_DESC(hystart_ack_delta, "spacing between ack's indicating train (msecs)");

/* Define delay_max paramater */
module_param(hystart_delay_max, int, 0644);
MODULE_PARM_DESC(hystart_delay_max, "Enable or disable upper bound clamping of HYSTART_DELAY_THRESH"
		"0: Clamp between HYSTART_DELAY_MIN and UINT_MAX"
		"1: Clamp between HYSTART_DELAY_MIN and HYSTART_DELAY_MAX");

/* BIC TCP Parameters */
struct bictcp {
	u32	cnt;		/* increase cwnd by 1 after ACKs */
	u32	last_max_cwnd;	/* last maximum snd_cwnd */
	u32	last_cwnd;	/* the last snd_cwnd */
	u32	last_time;	/* time when updated last_cwnd */
	u32	bic_origin_point;/* origin point of bic function */
	u32	bic_K;		/* time to origin point
				   from the beginning of the current epoch */
	u32	delay_min;	/* min delay (msec << 3) */
	u32	epoch_start;	/* beginning of an epoch */
	u32	ack_cnt;	/* number of acks */
	u32	tcp_cwnd;	/* estimated tcp cwnd */
	u16	unused;
	u8	sample_cnt;	/* number of samples to decide curr_rtt */
	u8	found;		/* the exit point is found? */
	u32	round_start;	/* beginning of each round */
	u32	end_seq;	/* end_seq of the round */
	u32	last_ack;	/* last time when the ACK spacing is close */
	u32	curr_rtt;	/* the minimum rtt of current round */
};

static inline void bictcp_reset(struct bictcp *ca)
{
	ca->cnt = 0;
	ca->last_max_cwnd = 0;
	ca->last_cwnd = 0;
	ca->last_time = 0;
	ca->bic_origin_point = 0;
	ca->bic_K = 0;
	ca->delay_min = 0;
	ca->epoch_start = 0;
	ca->ack_cnt = 0;
	ca->tcp_cwnd = 0;
	ca->found = 0;
}

static inline u32 bictcp_clock(void)
{
#if HZ < 1000
	return ktime_to_ms(ktime_get_real());
#else
	return jiffies_to_msecs(jiffies);
#endif
}

static inline void bictcp_hystart_reset(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);

	ca->round_start = ca->last_ack = bictcp_clock();
	ca->end_seq = tp->snd_nxt;
	ca->curr_rtt = 0;
	ca->sample_cnt = 0;
}

static void bictcp_init(struct sock *sk)
{
	struct bictcp *ca = inet_csk_ca(sk);

	bictcp_reset(ca);
	//printk(KERN_INFO "CUBIC: Current value of hystart_delay_max: %d ", hystart_delay_max);
	//total_pkts = 0;
	if (hystart)
		bictcp_hystart_reset(sk);

	if (!hystart && initial_ssthresh)
		tcp_sk(sk)->snd_ssthresh = initial_ssthresh;
}

static void bictcp_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
	if (event == CA_EVENT_TX_START) {
		struct bictcp *ca = inet_csk_ca(sk);
		u32 now = tcp_jiffies32;
		s32 delta;

		delta = now - tcp_sk(sk)->lsndtime;

		/* We were application limited (idle) for a while.
		 * Shift epoch_start to keep cwnd growth to cubic curve.
		 */
		if (ca->epoch_start && delta > 0) {
			ca->epoch_start += delta;
			if (after(ca->epoch_start, now))
				ca->epoch_start = now;
		}
		return;
	}
}

/* calculate the cubic root of x using a table lookup followed by one
 * Newton-Raphson iteration.
 * Avg err ~= 0.195%
 */
static u32 cubic_root(u64 a)
{
	u32 x, b, shift;
	/*
	 * cbrt(x) MSB values for x MSB values in [0..63].
	 * Precomputed then refined by hand - Willy Tarreau
	 *
	 * For x in [0..63],
	 *   v = cbrt(x << 18) - 1
	 *   cbrt(x) = (v[x] + 10) >> 6
	 */
	static const u8 v[] = {
		/* 0x00 */    0,   54,   54,   54,  118,  118,  118,  118,
		/* 0x08 */  123,  129,  134,  138,  143,  147,  151,  156,
		/* 0x10 */  157,  161,  164,  168,  170,  173,  176,  179,
		/* 0x18 */  181,  185,  187,  190,  192,  194,  197,  199,
		/* 0x20 */  200,  202,  204,  206,  209,  211,  213,  215,
		/* 0x28 */  217,  219,  221,  222,  224,  225,  227,  229,
		/* 0x30 */  231,  232,  234,  236,  237,  239,  240,  242,
		/* 0x38 */  244,  245,  246,  248,  250,  251,  252,  254,
	};

	b = fls64(a);
	if (b < 7) {
		/* a in [0..63] */
		return ((u32)v[(u32)a] + 35) >> 6;
	}

	b = ((b * 84) >> 8) - 1;
	shift = (a >> (b * 3));

	x = ((u32)(((u32)v[shift] + 10) << b)) >> 6;

	/*
	 * Newton-Raphson iteration
	 *                         2
	 * x    = ( 2 * x  +  a / x  ) / 3
	 *  k+1          k         k
	 */
	x = (2 * x + (u32)div64_u64(a, (u64)x * (u64)(x - 1)));
	x = ((x * 341) >> 10);
	return x;
}

/*
 * Compute congestion window to use.
 */
static inline void bictcp_update(struct bictcp *ca, u32 cwnd, u32 acked)
{
	u32 delta, bic_target, max_cnt;
	u64 offs, t;

	ca->ack_cnt += acked;	/* count the number of ACKed packets */

	if (ca->last_cwnd == cwnd &&
	    (s32)(tcp_jiffies32 - ca->last_time) <= HZ / 32)
		return;

	/* The CUBIC function can update ca->cnt at most once per jiffy.
	 * On all cwnd reduction events, ca->epoch_start is set to 0,
	 * which will force a recalculation of ca->cnt.
	 */
	if (ca->epoch_start && tcp_jiffies32 == ca->last_time)
		goto tcp_friendliness;

	ca->last_cwnd = cwnd;
	ca->last_time = tcp_jiffies32;

	if (ca->epoch_start == 0) {
		ca->epoch_start = tcp_jiffies32;	/* record beginning */
		ca->ack_cnt = acked;			/* start counting */
		ca->tcp_cwnd = cwnd;			/* syn with cubic */

		if (ca->last_max_cwnd <= cwnd) {
			ca->bic_K = 0;
			ca->bic_origin_point = cwnd;
		} else {
			/* Compute new K based on
			 * (wmax-cwnd) * (srtt>>3 / HZ) / c * 2^(3*bictcp_HZ)
			 */
			ca->bic_K = cubic_root(cube_factor
					       * (ca->last_max_cwnd - cwnd));
			ca->bic_origin_point = ca->last_max_cwnd;
		}
	}

	/* cubic function - calc*/
	/* calculate c * time^3 / rtt,
	 *  while considering overflow in calculation of time^3
	 * (so time^3 is done by using 64 bit)
	 * and without the support of division of 64bit numbers
	 * (so all divisions are done by using 32 bit)
	 *  also NOTE the unit of those veriables
	 *	  time  = (t - K) / 2^bictcp_HZ
	 *	  c = bic_scale >> 10
	 * rtt  = (srtt >> 3) / HZ
	 * !!! The following code does not have overflow problems,
	 * if the cwnd < 1 million packets !!!
	 */

	t = (s32)(tcp_jiffies32 - ca->epoch_start);
	t += msecs_to_jiffies(ca->delay_min >> 3);
	/* change the unit from HZ to bictcp_HZ */
	t <<= BICTCP_HZ;
	do_div(t, HZ);

	if (t < ca->bic_K)		/* t - K */
		offs = ca->bic_K - t;
	else
		offs = t - ca->bic_K;

	/* c/rtt * (t-K)^3 */
	delta = (cube_rtt_scale * offs * offs * offs) >> (10+3*BICTCP_HZ);
	if (t < ca->bic_K)                            /* below origin*/
		bic_target = ca->bic_origin_point - delta;
	else                                          /* above origin*/
		bic_target = ca->bic_origin_point + delta;

	/* cubic function - calc bictcp_cnt*/
	if (bic_target > cwnd) {
		ca->cnt = cwnd / (bic_target - cwnd);
	} else {
		ca->cnt = 100 * cwnd;              /* very small increment*/
	}

	/*
	 * The initial growth of cubic function may be too conservative
	 * when the available bandwidth is still unknown.
	 */
	if (ca->last_max_cwnd == 0 && ca->cnt > 20)
		ca->cnt = 20;	/* increase cwnd 5% per RTT */

tcp_friendliness:
	/* TCP Friendly */
	if (tcp_friendliness) {
		u32 scale = beta_scale;

		delta = (cwnd * scale) >> 3;
		while (ca->ack_cnt > delta) {		/* update tcp cwnd */
			ca->ack_cnt -= delta;
			ca->tcp_cwnd++;
		}

		if (ca->tcp_cwnd > cwnd) {	/* if bic is slower than tcp */
			delta = ca->tcp_cwnd - cwnd;
			max_cnt = cwnd / delta;
			if (ca->cnt > max_cnt)
				ca->cnt = max_cnt;
		}
	}

	/* The maximum rate of cwnd increase CUBIC allows is 1 packet per
	 * 2 packets ACKed, meaning cwnd grows at 1.5x per RTT.
	 */
	ca->cnt = max(ca->cnt, 2U);
}

static void bictcp_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);

	if (!tcp_is_cwnd_limited(sk))
		return;

	if (tcp_in_slow_start(tp)) {
		if (hystart && after(ack, ca->end_seq))
			bictcp_hystart_reset(sk);
		acked = tcp_slow_start(tp, acked);
		if (!acked)
			return;
	}
	bictcp_update(ca, tp->snd_cwnd, acked);
	tcp_cong_avoid_ai(tp, ca->cnt, acked);
}

static u32 bictcp_recalc_ssthresh(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);
    uint16_t s0, s1;
    uint16_t de0, de1;
	int port;
    int desport;
    int t2;
	s0 = (tp->inet_conn.icsk_inet.inet_sport & 0x00ff) << 8u;
	s1 = (tp->inet_conn.icsk_inet.inet_sport & 0xff00) >> 8u;
	port = s0 | s1;
    /////////////new changes/////////////////////////////////////////////////////////////////
    de0 = (tp->inet_conn.icsk_inet.inet_dport & 0x00ff) << 8u;
	de1 = (tp->inet_conn.icsk_inet.inet_dport & 0xff00) >> 8u;
	desport = de0 | de1;

	ca->epoch_start = 0;	/* end of epoch */

	/* Wmax and fast convergence */
	if (tp->snd_cwnd < ca->last_max_cwnd && fast_convergence)
		ca->last_max_cwnd = (tp->snd_cwnd * (BICTCP_BETA_SCALE + beta))
			/ (2 * BICTCP_BETA_SCALE);
	else
		ca->last_max_cwnd = tp->snd_cwnd;
    
    //printk(KERN_ALERT "CUBIC STATS (%hu, %hu): cwnd2: $%u\n", port, desport, tp->snd_cwnd);
  //  printk(KERN_ALERT "CUBIC STATS (%hu, %hu): SSthresh2: $%u\n", port, desport, tp->snd_ssthresh);
    t2 = 1;
    if (t2 == 1) {
        if (tp->snd_cwnd >= tp->snd_ssthresh) {
    		printk(KERN_ALERT "CUBIC INFO(%hu, %hu): EXIT SS with CWIND= %u and SSThRESH= %u \n", port, desport, tp->snd_cwnd, tp->snd_ssthresh);
        }
    }
	return max((tp->snd_cwnd * beta) / BICTCP_BETA_SCALE, 2U);
}

static void bictcp_state(struct sock *sk, u8 new_state)
{
	if (new_state == TCP_CA_Loss) {
		bictcp_reset(inet_csk_ca(sk));
		bictcp_hystart_reset(sk);
	}
}

static void hystart_update(struct sock *sk, u32 delay)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);
	//uint16_t b0, b1;
	//int port;
        //b0 = (tp->inet_conn.icsk_inet.inet_sport & 0x00ff) << 8u;
	//b1 = (tp->inet_conn.icsk_inet.inet_sport & 0xff00) >> 8u;
	//port = b0 | b1;
	//printk(KERN_INFO "CUBIC: in hystart_update");	
	//printk(KERN_INFO "CUBIC (%hu): Sample count: $%d\n", port, ca->sample_cnt);
	//printk(KERN_INFO "CUBIC STAT: Delay: %d\n", (delay >> 3));
	//printk(KERN_INFO "CUBIC (%hu): curr RTT: $%d\n", port, (ca->curr_rtt >> 3));
	//printk(KERN_INFO "CUBIC (%hu): min RTT: $%d\n", port, (ca->delay_min >> 3));
	//printk(KERN_INFO "CUBIC (%hu): delay thresh: $%d\n", port, (HYSTART_DELAY_THRESH(ca->delay_min >> 3) >> 3));

	if (ca->found & hystart_detect)
		return;

	if (hystart_detect & HYSTART_ACK_TRAIN) {
		u32 now = bictcp_clock();

		/* first detection parameter - ack-train detection */
		if ((s32)(now - ca->last_ack) <= hystart_ack_delta) {
			ca->last_ack = now;
			if ((s32)(now - ca->round_start) > ca->delay_min >> 4) {
				ca->found |= HYSTART_ACK_TRAIN;
				NET_INC_STATS(sock_net(sk),
				      LINUX_MIB_TCPHYSTARTTRAINDETECT);
				NET_ADD_STATS(sock_net(sk),
					      LINUX_MIB_TCPHYSTARTTRAINCWND,
					      tp->snd_cwnd);
				tp->snd_ssthresh = tp->snd_cwnd;
			}
		}
	}

	if (hystart_detect & HYSTART_DELAY) {
		// printk(KERN_INFO "CUBIC: Looking at delays");
		// printk(KERN_INFO "CUBIC: Current value of hystart_delay_max: %d", hystart_delay_max);
		// printk(KERN_INFO "CUBIC: Current value of ca->sample_cnt: %d", ca->sample_cnt);
		/* obtain the minimum delay of more than sampling packets */
		if (ca->curr_rtt > delay)
			ca->curr_rtt = delay;
		if (ca->sample_cnt < HYSTART_MIN_SAMPLES) {
			// printk(KERN_INFO "CUBIC: Sample count less than min samples");
			if (ca->curr_rtt == 0 || ca->curr_rtt > delay)
				ca->curr_rtt = delay;

			ca->sample_cnt++;
		} else {
			//printk(KERN_INFO "CUBIC EVENT (%hu): Evaluating delay increase and resetting\n", port);
			// printk(KERN_INFO "CUBIC: Current min RTT is: %d\n", ca->delay_min);
			// printk(KERN_INFO "CUBIC: Current value of HYSTART_DELAY_THRESH: %d\n", HYSTART_DELAY_THRESH(ca->delay_min >> 3));
			if (ca->curr_rtt > ca->delay_min +
			    HYSTART_DELAY_THRESH(ca->delay_min >> 3)) {
				//printk(KERN_INFO "CUBIC (%hu): Exit due to delay detect\n", port);
				ca->found |= HYSTART_DELAY;
				NET_INC_STATS(sock_net(sk),
					      LINUX_MIB_TCPHYSTARTDELAYDETECT);
				NET_ADD_STATS(sock_net(sk),
					      LINUX_MIB_TCPHYSTARTDELAYCWND,
					      tp->snd_cwnd);
				tp->snd_ssthresh = tp->snd_cwnd;
			}
		}
	}
}

/* Track delayed acknowledgment ratio using sliding window
 * ratio = (15*ratio + sample) / 16
 */
static void bictcp_acked(struct sock *sk, const struct ack_sample *sample)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);
    //const struct rate_sample *rs; 
	u32 delay;
	uint16_t b0, b1;
    uint16_t d0, d1;
	int port;
    int desport;
    int t;
	long long variance;
	long sdev;
	u32 start;
	u64 mid;
	u64 end;
	static u32 rtt_sdev_ms = 0;
	/* Some calls are for duplicates without timetamps */
	if (sample->rtt_us < 0)
		return;

	/* Discard delay samples right after fast recovery */
	if (ca->epoch_start && (s32)(tcp_jiffies32 - ca->epoch_start) < HZ)
		return;

	delay = (sample->rtt_us << 3) / USEC_PER_MSEC;
	if (delay == 0)
		delay = 1;
	
	if (tp->sdev_stats.num_packets > 0){
		// Convert M2 into varinace in MS
		variance = (tp->sdev_stats.m2_rtt_ms / tp->sdev_stats.num_packets);
	}else{
		variance  = 0;
	}
	if (variance == 0 || variance == 1){
		sdev = variance;
	}else if (variance < 0){
		printk(KERN_INFO "CUBIC WARNING: varinace was measured to be negative");
	}else{
		// Do Binary Search for floor(sqrt(variance))
		start = 1;
		end = variance >> 1;
		while (start <= end) {
			
			//if (rtt_sdev_ms){
			//	mid = rtt_sdev_ms;
			//	rtt_sdev_ms = 0;
			//} else{
				mid = (start + end) >> 1;
			//}
			
			// If x is a perfect square
			if (mid * mid == variance){
				sdev = mid;
				break;
			}
			// if variance is larger than mid*mid move towards the sqrt of variance
			if(mid <= variance / mid){
				start = mid+1;
				sdev = mid;
			}else{ 
				// If mid*mid is greater than x
				end = mid - 1;
			}
    		}
		//printk(KERN_INFO "CUBIC STATS (%hu): Start: $%d", port, start);
		//printk(KERN_INFO "CUBIC STATS (%hu): M2: $%lld", port, tp->sdev_stats.m2_rtt_us);
	}
	rtt_sdev_ms = sdev;

	// printk(KERN_INFO "CUBIC: current value of ca->found %d", ca->found);
	b0 = (tp->inet_conn.icsk_inet.inet_sport & 0x00ff) << 8u;
	b1 = (tp->inet_conn.icsk_inet.inet_sport & 0xff00) >> 8u;
	port = b0 | b1;
    /////////////new changes/////////////////////////////////////////////////////////////////
    d0 = (tp->inet_conn.icsk_inet.inet_dport & 0x00ff) << 8u;
	d1 = (tp->inet_conn.icsk_inet.inet_dport & 0xff00) >> 8u;
	desport = d0 | d1;
    
    t = 1;

    if (t == 1) {
        ///////////////////////////////////////////////////////////////////////////////////////
	    //printk(KERN_INFO "CUBIC (%hu): packets since start: $%d\n", port, total_pkts++);
        //printk(KERN_INFO "CUBIC STATS (%hu): M2: $%lld", port, (tp->sdev_stats.m2_rtt_ns / (USEC_PER_MSEC * USEC_PER_MSEC)));
        //printk(KERN_INFO "CUBIC STATS (%hu): packets in flight: $%d\n", port, tp->packets_out);
        //printk(KERN_INFO "CUBIC (%hu): Sample count: $%d\n", port, ca->sample_cnt);
    	//printk(KERN_INFO "CUBIC (%hu): curr RTT: $%d\n", port, (ca->curr_rtt >> 3));
    	//printk(KERN_INFO "CUBIC (%hu): min RTT: $%d\n", port, (ca->delay_min >> 3));
    	//printk(KERN_INFO "CUBIC (%hu): delay thresh: $%d\n", port, (HYSTART_DELAY_THRESH(ca->delay_min >> 3) >> 3));
    	//printk(KERN_INFO "CUBIC (%hu): Smoothed RTT: $%ld\n", port, (tp->srtt_us/USEC_PER_MSEC));
    	//printk(KERN_INFO "CUBIC (%hu): Smoothed mdev: $%ld\n", port, (tp->rttvar_us/USEC_PER_MSEC));
    	//printk(KERN_INFO "CUBIC (%hu): Max mdev: $%ld\n", port, (tp->mdev_max_us/USEC_PER_MSEC));
    	printk(KERN_INFO "CUBIC STATS (%hu, %hu): sample RTT: $%ld\n", port, desport, (sample->rtt_us / USEC_PER_MSEC));
    	printk(KERN_INFO "CUBIC STATS (%hu, %hu): Running avg: $%ld", port, desport, (tp->sdev_stats.mean_rtt_us / USEC_PER_MSEC));
    	printk(KERN_INFO "CUBIC STATS (%hu, %hu): sdev: $%ld\n", port, desport, (sdev));
    	printk(KERN_INFO "CUBIC STATS (%hu, %hu): variance: $%lld\n", port, desport, (variance));
    	printk(KERN_INFO "CUBIC STATS (%hu, %hu): count: $%ld\n", port, desport, tp->sdev_stats.num_packets);
    	printk(KERN_INFO "CUBIC STATS (%hu, %hu): m2: $%lld\n", port, desport, tp->sdev_stats.m2_rtt_ms);
    	printk(KERN_INFO "CUBIC STATS (%hu, %hu): cwnd: $%u\n", port, desport, tp->snd_cwnd);
        /////////////new changes/////////////////////////////////////////////////////////////////
        printk(KERN_INFO "CUBIC STATS (%hu, %hu): SSthresh: $%u\n", port, desport, tp->snd_ssthresh);
        /////////////////////////////////////////////////////////////////////////////////////////
    	printk(KERN_INFO "CUBIC STATS (%hu, %hu): pkts_acked: $%d\n", port, desport, sample->pkts_acked);
    	printk(KERN_INFO "CUBIC STATS (%hu, %hu): mss: $%d\n", port, desport, tp->mss_cache);
        printk(KERN_INFO "CUBIC (%hu, %hu): Medium Deviation: $%ld\n", port, desport, (tp->mdev_us/ USEC_PER_MSEC));
        /////////////new changes/////////////////////////////////////////////////////////////////
        printk(KERN_INFO "CUBIC STATS (%hu, %hu): pkt_loss: $%u\n", port, desport, tp->lost_out);
        printk(KERN_INFO "CUBIC STATS (%hu, %hu): retrans_seg: $%u\n", port, desport, tp->retrans_out);
        printk(KERN_INFO "CUBIC STATS (%hu, %hu): Bytes-sent: $%llu\n", port, desport, tp->bytes_sent);
        printk(KERN_INFO "CUBIC STATS (%hu, %hu): Bytes-acked: $%llu\n", port, desport, tp->bytes_acked);
        //printk(KERN_INFO "CUBIC STATS (%hu, %hu): seq_num: $%u\n", port, desport, tp->data_segs_out);
        printk(KERN_INFO "CUBIC STATS (%hu, %hu): seq_num2: $%u\n", port, desport, tp->snd_nxt);
        printk(KERN_INFO "CUBIC STATS (%hu, %hu): delivery_rate: $%u\n", port, desport, tp->rate_delivered);
        //printk(KERN_INFO "CUBIC STATS (%hu, %hu): delivery_rate: $%u\n", port, desport, rs->delivered);
        printk(KERN_INFO "CUBIC STATS (%hu, %hu): deliveredpkts: $%u\n", port, desport, tp->delivered);
        printk(KERN_INFO "CUBIC STATS (%hu, %hu): packets in flight: $%d\n", port, desport, tp->packets_out);   
        printk(KERN_INFO "CUBIC STATS (%hu, %hu): sackedout: $%u\n", port, desport, tp->sacked_out);
        printk(KERN_INFO "CUBIC STATS (%hu, %hu): sequence-of-ack: $%u\n", port, desport, tp->pushed_seq);
	    printk(KERN_INFO "CUBIC STATS (%hu, %hu): The end: $%u //////////////////////////////////////\n", port, desport, tp->snd_una);

        if (tcp_in_slow_start(tp))
    	    printk(KERN_INFO "CUBIC INFO(%hu, %hu): In slow start with CWIND= %u and SSThRESH= %u\n", port, desport, tp->snd_cwnd, tp->snd_ssthresh);
   
    }
        /////////////////////////////////////////////////////////////////////////////////////////
    if (tp->snd_cwnd >= tp->snd_ssthresh) {
    		printk(KERN_INFO "CUBIC INFO(%hu, %hu): Exit slow start with CWIND= %u and SSThRESH= %u \n", port, desport, tp->snd_cwnd, tp->snd_ssthresh);
    }   
    
    
    
	//if (ca->found == 2)
	//	printk(KERN_INFO "CUBIC (%hu): Exit due to delay detect\n", port);
	//printk(KERN_INFO "CUBIC STATS: sport: %hu\n", port);
	/* hystart triggers when cwnd is larger than some threshold */
	if (hystart && tcp_in_slow_start(tp) &&
         tp->snd_cwnd >= hystart_low_window){
		//printk(KERN_INFO "CUBIC: sample RTT: %ld\n", (sample->rtt_us / USEC_PER_MSEC));
		//printk(KERN_INFO "CUBIC: socket RTT: %d\n", (ca->curr_rtt));
		hystart_update(sk, delay);
	}
}

static struct tcp_congestion_ops cubictcp __read_mostly = {
	.init		= bictcp_init,
	.ssthresh	= bictcp_recalc_ssthresh,
	.cong_avoid	= bictcp_cong_avoid,
	.set_state	= bictcp_state,
	.undo_cwnd	= tcp_reno_undo_cwnd,
	.cwnd_event	= bictcp_cwnd_event,
	.pkts_acked     = bictcp_acked,
	.owner		= THIS_MODULE,
	.name		= "cubic",
};

static int __init cubictcp_register(void)
{
	BUILD_BUG_ON(sizeof(struct bictcp) > ICSK_CA_PRIV_SIZE);

	/* Precompute a bunch of the scaling factors that are used per-packet
	 * based on SRTT of 100ms
	 */

	beta_scale = 8*(BICTCP_BETA_SCALE+beta) / 3
		/ (BICTCP_BETA_SCALE - beta);

	cube_rtt_scale = (bic_scale * 10);	/* 1024*c/rtt */

	/* calculate the "K" for (wmax-cwnd) = c/rtt * K^3
	 *  so K = cubic_root( (wmax-cwnd)*rtt/c )
	 * the unit of K is bictcp_HZ=2^10, not HZ
	 *
	 *  c = bic_scale >> 10
	 *  rtt = 100ms
	 *
	 * the following code has been designed and tested for
	 * cwnd < 1 million packets
	 * RTT < 100 seconds
	 * HZ < 1,000,00  (corresponding to 10 nano-second)
	 */

	/* 1/c * 2^2*bictcp_HZ * srtt */
	cube_factor = 1ull << (10+3*BICTCP_HZ); /* 2^40 */

	/* divide by bic_scale and by constant Srtt (100ms) */
	do_div(cube_factor, bic_scale * 10);

	return tcp_register_congestion_control(&cubictcp);
}

static void __exit cubictcp_unregister(void)
{
	tcp_unregister_congestion_control(&cubictcp);
}

module_init(cubictcp_register);
module_exit(cubictcp_unregister);

MODULE_AUTHOR("Sangtae Ha, Stephen Hemminger");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CUBIC TCP");
MODULE_VERSION("2.3");
