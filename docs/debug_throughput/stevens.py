#!/usr/bin/env python3
import matplotlib.pyplot as plot
import numpy
import sys

# edit this to skip any records you don't want to graph
skip_records = ['probe_rtt', 'probe_rtt_done', 'probe_bw', 'drain']

# don't edit these


time = []
sent = []
acked = []

vlines = []


def processFile(path_to_file):
    prev_time = None
    t0 = None
    
    with open(path_to_file, 'r') as f:
        for line in f:
            args = line.split()

            if line.startswith("#"):
                continue;
            if len(args) < 2:
                continue

            t = int(args[0])

            if t0 == None or t < t0:
                t0 = t

            if args[1] in skip_records:
                continue

            if args[1] == "tp":
                s = int(args[2])
                a = int(args[3])

                #if t != prev_time: # aggregate same times, the last entry will have most up to date
                time.append(t - t0)
                sent.append(s)
                acked.append(a)
                    #prev_time = t
            else:
                # for now, all non-tp args get a vertical line marker
                vlines.append((t-t0, args[1]))

    print("processed %d records\n" % len(time))


if (len(sys.argv) < 2):
    print("usage: ./stevens.py stream1.dat")
    exit(1)


processFile(sys.argv[1])

print("time entries: %d" % len(time))
print("vlines=", vlines)

t0 = time[0]

time = [ e - t0 for e in time ]

figure, axes = plot.subplots()

red = "tab:red"
blue = "tab:blue"

axes.set_xlabel('time (ms)')
axes.set_ylabel('sent', color=blue)

axes.plot(time, sent, color=blue, label="sent")

d = axes.twinx()
d.set_ylabel('delivered', color=red)
d.plot(time, acked, color=red, label="acked")


colors = 'bgrcmk'
color_index=0

for (x, label) in vlines:
    print("x=", x, "label=", label)
    plot.axvline(x=x, color=colors[color_index], ls="--", label=label)
    color_index = (color_index + 1) % len(colors)

axes.legend()
d.legend()

figure.tight_layout()


plot.show()
