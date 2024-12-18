set termopt noenhanced
set title sprintf("libUDX\n[%s]", ARG1)
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

D = ARG1

plot D using 1:($2 / 1024 / 1024) with lines title "Mbit/s", \
		 D using 1:($3 / 1024 / 1024) with lines title "Avg Mbit/s", \
		 D using 1:4 axes x1y2 with lines title "packets", \
		 D using 1:6 axes x1y2 with lines title "load (ms)", \
                 D using 1:7 axes x1y2 with lines title "K-drop" , \
		 D using 1:($9 * 100) with lines title "thread buffer cap %", # \
		 #D using 1:11 axes x1y2 with lines title "n-drains"
		 #D using 1:($10 / $11) axes x1y2 with lines title "packets buffered"
