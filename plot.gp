set termopt noenhanced

set title sprintf("receiver w/load sim\n[%s]", ARG1)
# set xlabel "Time"
set ylabel "Speed (Mbits / second)"
set y2label "Count (millis | packets)"

set xrange # [0:60]
set yrange [0:100]
set y2range [0:3000]

set y2tics
set ytics nomirror

# set key left top
set key outside bottom
set key horizontal
# set bmargin at screen 0.1

# columns
recv_mbps = 2
avg_recv_mbps = 3
packets = 4
packets_per_second = 5
load_ms = 6
k_drop = 7
t_drop = 8
trxq_load = 9
trxq_packets = 10
trxq_drains = 11
runtime = 12
seq = 13
j_interval = 14
j_sat = 15
delta = 16

D = ARG1
X=runtime

plot D using X:($2 / 1024 / 1024) with lines title "Mbit/s", \
		 D using X:($3 / 1024 / 1024) with lines title "Avg Mbit/s", \
		 D using X:4 axes x1y2 with lines title "packets", \
		 D using X:(($6 / $16) * 100) with lines title "load (%)", \
                 D using X:7 axes x1y2 with lines title "K-drop" , \
		 D using X:($9 * 100) with lines title "thread buffer cap %", # \
		 #D using 1:11 axes x1y2 with lines title "n-drains"
		 #D using 1:($10 / $11) axes x1y2 with lines title "packets buffered"
pause -1


		 # D using X:6 axes x1y2 with lines title "load (ms)", \
