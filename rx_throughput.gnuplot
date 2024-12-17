set termopt noenhanced
set title sprintf("libUDX reciever end\n[%s]", ARG1)
set xlabel "Round"
set ylabel "Rate (mega bits / second)"
set y2label "Count"

set xrange # [0:60]
set yrange # [0:3]
set y2range # [0:3000]

set y2tics
set ytics nomirror

set key left top

D = ARG1

plot D using 1:($2 / 1024 / 1024) with lines title "Mbit/s", \
		 D using 1:($3 / 1024 / 1024) with lines title "Avg Mbit/s", \
		 D using 1:4 axes x1y2 with lines title "packets", \
		 D using 1:6 axes x1y2 with lines title "load (ms)", \
		 D using 1:($10 / $11) axes x1y2 with lines title "packets buffered"
