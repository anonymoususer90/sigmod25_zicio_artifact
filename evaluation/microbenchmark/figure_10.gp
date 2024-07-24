set term epscairo enhanced dashed size 8in, 4.5in font "Arial, " background rgb 'white'
set output "figure_10.eps"

f(x) = 0.075 + ((x+0.5) * 0.895 / 4)

fx(x, w) = x - 0.046 + w * 0.77
fy(y) = ((29 * 0.06) / 0.44) + (y * ((29 * 0.2) / 0.44) / 2.2)

set multiplot

set grid y
set style data histogram

set style fill solid 1.00 border -1

unset xtics

unset xlabel
set ylabel "Latancy (x100 sec, Avg)" font "Arial, 24" offset -2, -8
set y2label "16 GiB Memory" font "Arial Bold, 20" offset 0.3, -1.75

set style line 1 lw 3 lt 6 lc rgb "black"
set pointintervalbox 0

set boxwidth 0.1

set lmargin at screen 0.075
set rmargin at screen 0.97
set tmargin at screen 1. - 0.11
set bmargin at screen 1. - 0.55

set xrange [-0.5:3.5]
set yrange [0:29]
set ytics 4,4,16 font ", 20" scale 0.0

plot "results_0_small_16/zicio_X_0.dat" using ($0-0.30):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
	 "results_0_small_16/zicio_X_0.dat" using ($0-0.30):($5/100) with boxes fs solid lt rgb "red" notitle, \
	 "results_0_small_16/zicio_X_0.dat" using ($0-0.30):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle, \
	 \
	 "results_0_small_16/zicio_O_0.dat" using ($0-0.20):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
	 "results_0_small_16/zicio_O_0.dat" using ($0-0.20):($5/100) with boxes fs solid lt rgb "red" notitle, \
	 "results_0_small_16/zicio_O_0.dat" using ($0-0.20):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle, \
	 \
	 "results_0_small_16/pread_X_0_4096.dat" using ($0-0.10):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
	 "results_0_small_16/pread_X_0_4096.dat" using ($0-0.10):($5/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_small_16/pread_X_0_4096.dat" using ($0-0.10):($5/100) with boxes fs pattern 7 transparent lt rgb "black" notitle, \
	 "results_0_small_16/pread_X_0_4096.dat" using ($0-0.10):(($7+$8+$9)/100) with boxes fs solid lt rgb "#666666" notitle, \
	 "results_0_small_16/pread_X_0_4096.dat" using ($0-0.10):(($8+$9)/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_small_16/pread_X_0_4096.dat" using ($0-0.10):(($8+$9)/100) with boxes fs pattern 14 transparent lt rgb "black" notitle, \
	 "results_0_small_16/pread_X_0_4096.dat" using ($0-0.10):(($9)/100) with boxes fs solid lt rgb "#000000" notitle, \
	 "results_0_small_16/pread_X_0_4096.dat" using ($0-0.10):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle, \
	 \
	 "results_0_small_16/pread_X_0_8192.dat" using ($0-0.00):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
	 "results_0_small_16/pread_X_0_8192.dat" using ($0-0.00):($5/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_small_16/pread_X_0_8192.dat" using ($0-0.00):($5/100) with boxes fs pattern 7 transparent lt rgb "black" notitle, \
	 "results_0_small_16/pread_X_0_8192.dat" using ($0-0.00):(($7+$8+$9)/100) with boxes fs solid lt rgb "#666666" notitle, \
	 "results_0_small_16/pread_X_0_8192.dat" using ($0-0.00):(($8+$9)/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_small_16/pread_X_0_8192.dat" using ($0-0.00):(($8+$9)/100) with boxes fs pattern 14 transparent lt rgb "black" notitle, \
	 "results_0_small_16/pread_X_0_8192.dat" using ($0-0.00):(($9)/100) with boxes fs solid lt rgb "#000000" notitle, \
	 "results_0_small_16/pread_X_0_8192.dat" using ($0-0.00):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle, \
	 \
	 "results_0_small_16/pread_X_0_16384.dat" using ($0+0.10):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
	 "results_0_small_16/pread_X_0_16384.dat" using ($0+0.10):($5/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_small_16/pread_X_0_16384.dat" using ($0+0.10):($5/100) with boxes fs pattern 7 transparent lt rgb "black" notitle, \
	 "results_0_small_16/pread_X_0_16384.dat" using ($0+0.10):(($7+$8+$9)/100) with boxes fs solid lt rgb "#666666" notitle, \
	 "results_0_small_16/pread_X_0_16384.dat" using ($0+0.10):(($8+$9)/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_small_16/pread_X_0_16384.dat" using ($0+0.10):(($8+$9)/100) with boxes fs pattern 14 transparent lt rgb "black" notitle, \
	 "results_0_small_16/pread_X_0_16384.dat" using ($0+0.10):(($9)/100) with boxes fs solid lt rgb "#000000" notitle, \
	 "results_0_small_16/pread_X_0_16384.dat" using ($0+0.10):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle, \
	 \
	 "results_0_small_16/pread_O_0_4096.dat" using ($0+0.20):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
	 "results_0_small_16/pread_O_0_4096.dat" using ($0+0.20):($5/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_small_16/pread_O_0_4096.dat" using ($0+0.20):($5/100) with boxes fs pattern 7 transparent lt rgb "black" notitle, \
	 "results_0_small_16/pread_O_0_4096.dat" using ($0+0.20):(($7+$8+$9)/100) with boxes fs solid lt rgb "#666666" notitle, \
	 "results_0_small_16/pread_O_0_4096.dat" using ($0+0.20):(($8+$9)/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_small_16/pread_O_0_4096.dat" using ($0+0.20):(($8+$9)/100) with boxes fs pattern 14 transparent lt rgb "black" notitle, \
	 "results_0_small_16/pread_O_0_4096.dat" using ($0+0.20):(($9)/100) with boxes fs solid lt rgb "#000000" notitle, \
	 "results_0_small_16/pread_O_0_4096.dat" using ($0+0.20):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle, \
	 \
	 "results_0_small_16/pread_O_0_8192.dat" using ($0+0.30):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
	 "results_0_small_16/pread_O_0_8192.dat" using ($0+0.30):($5/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_small_16/pread_O_0_8192.dat" using ($0+0.30):($5/100) with boxes fs pattern 7 transparent lt rgb "black" notitle, \
	 "results_0_small_16/pread_O_0_8192.dat" using ($0+0.30):(($7+$8+$9)/100) with boxes fs solid lt rgb "#666666" notitle, \
	 "results_0_small_16/pread_O_0_8192.dat" using ($0+0.30):(($8+$9)/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_small_16/pread_O_0_8192.dat" using ($0+0.30):(($8+$9)/100) with boxes fs pattern 14 transparent lt rgb "black" notitle, \
	 "results_0_small_16/pread_O_0_8192.dat" using ($0+0.30):(($9)/100) with boxes fs solid lt rgb "#000000" notitle, \
	 "results_0_small_16/pread_O_0_8192.dat" using ($0+0.30):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle, \
	 \
	 "results_0_small_16/pread_O_0_16384.dat" using ($0+0.40):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
	 "results_0_small_16/pread_O_0_16384.dat" using ($0+0.40):($5/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_small_16/pread_O_0_16384.dat" using ($0+0.40):($5/100) with boxes fs pattern 7 transparent lt rgb "black" notitle, \
	 "results_0_small_16/pread_O_0_16384.dat" using ($0+0.40):(($7+$8+$9)/100) with boxes fs solid lt rgb "#666666" notitle, \
	 "results_0_small_16/pread_O_0_16384.dat" using ($0+0.40):(($8+$9)/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_small_16/pread_O_0_16384.dat" using ($0+0.40):(($8+$9)/100) with boxes fs pattern 14 transparent lt rgb "black" notitle, \
	 "results_0_small_16/pread_O_0_16384.dat" using ($0+0.40):(($9)/100) with boxes fs solid lt rgb "#000000" notitle, \
	 "results_0_small_16/pread_O_0_16384.dat" using ($0+0.40):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle

unset ylabel
unset y2label

set tmargin at screen 1. - 0.29
set bmargin at screen 1. - 0.49
do for [pos=0:3] {
	set yrange[0:2.2]
		
	set lmargin at screen f(pos-0.35)
	set rmargin at screen f(pos+0.15)-0.01

	unset grid
	unset ytics
	set object rect from screen f(pos-0.35), 1.-0.49 to screen f(pos+0.15)-0.01, 1.-0.29 behind fillcolor rgb 'white' fillstyle solid noborder
	plot NaN notitle
	unset object

	set grid y
	set xrange [pos-0.4:pos+0.4-0.2]
	set ytics 1,1,3 font ", 18" scale 0.0

	plot "<(sed -n '".(pos+1)."p' results_0_small_16/zicio_X_0.dat)" using (pos-0.30):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/zicio_X_0.dat)" using (pos-0.30):($5/100) with boxes fs solid lt rgb "red" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/zicio_X_0.dat)" using (pos-0.30):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle, \
		 \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/zicio_O_0.dat)" using (pos-0.20):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/zicio_O_0.dat)" using (pos-0.20):($5/100) with boxes fs solid lt rgb "red" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/zicio_O_0.dat)" using (pos-0.20):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle, \
		 \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_4096.dat)" using (pos-0.10):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_4096.dat)" using (pos-0.10):($5/100) with boxes fs solid lt rgb "#cccccc" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_4096.dat)" using (pos-0.10):($5/100) with boxes fs pattern 7 transparent lt rgb "black" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_4096.dat)" using (pos-0.10):(($7+$8+$9)/100) with boxes fs solid lt rgb "#666666" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_4096.dat)" using (pos-0.10):(($8+$9)/100) with boxes fs solid lt rgb "#cccccc" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_4096.dat)" using (pos-0.10):(($8+$9)/100) with boxes fs pattern 14 transparent lt rgb "black" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_4096.dat)" using (pos-0.10):(($9)/100) with boxes fs solid lt rgb "#000000" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_4096.dat)" using (pos-0.10):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle, \
		 \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_8192.dat)" using (pos-0.00):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_8192.dat)" using (pos-0.00):($5/100) with boxes fs solid lt rgb "#cccccc" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_8192.dat)" using (pos-0.00):($5/100) with boxes fs pattern 7 transparent lt rgb "black" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_8192.dat)" using (pos-0.00):(($7+$8+$9)/100) with boxes fs solid lt rgb "#666666" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_8192.dat)" using (pos-0.00):(($8+$9)/100) with boxes fs solid lt rgb "#cccccc" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_8192.dat)" using (pos-0.00):(($8+$9)/100) with boxes fs pattern 14 transparent lt rgb "black" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_8192.dat)" using (pos-0.00):(($9)/100) with boxes fs solid lt rgb "#000000" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_8192.dat)" using (pos-0.00):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle, \
		 \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_16384.dat)" using (pos+0.10):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_16384.dat)" using (pos+0.10):($5/100) with boxes fs solid lt rgb "#cccccc" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_16384.dat)" using (pos+0.10):($5/100) with boxes fs pattern 7 transparent lt rgb "black" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_16384.dat)" using (pos+0.10):(($7+$8+$9)/100) with boxes fs solid lt rgb "#666666" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_16384.dat)" using (pos+0.10):(($8+$9)/100) with boxes fs solid lt rgb "#cccccc" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_16384.dat)" using (pos+0.10):(($8+$9)/100) with boxes fs pattern 14 transparent lt rgb "black" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_16384.dat)" using (pos+0.10):(($9)/100) with boxes fs solid lt rgb "#000000" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_small_16/pread_X_0_16384.dat)" using (pos+0.10):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle
}

set lmargin at screen 0.075
set rmargin at screen 0.97
set tmargin at screen 1. - 0.11
set bmargin at screen 1. - 0.55

set xrange [-0.5:3.5]
set yrange [0:29]
unset ytics
set noborder

plot "<(sed -n '1p' results_0_small_16/zicio_X_0.dat)" using (fx(0,-0.30-0.025)):(25):(0):(fy($2/100)-25) with vectors filled head lw 3 lc "black" notitle, \
	 "" using (fx(0,-0.30-0.025)-0.03):(25 + 2):(sprintf('{/Times-New-Roman-Bold=20 U{/=16 ZIC}IO} + {/Times-New-Roman-Bold=20 K{/=16 ZIC}IO}')) with labels left font "Arial Bold, 20" notitle, \
	 "<(sed -n '1p' results_0_small_16/zicio_O_0.dat)" using (fx(0,-0.20-0.025)):(22):(0):(fy($2/100)-22) with vectors filled head lw 3 lc "black" notitle, \
	 "" using (fx(0,-0.20-0.025)-0.03):(22 + 2):(sprintf('{/Times-New-Roman-Bold=20 U{/=16 ZIC}IO} + {/Times-New-Roman-Bold=20 SK{/=16 ZIC}IO}')) with labels left font "Arial Bold, 20" notitle, \
	 "<(sed -n '2p' results_0_small_16/pread_X_0_4096.dat)" using (fx(1,-0.10-0.025)):(25):(0):(fy($2/100)-25) with vectors filled head lw 3 lc "black" notitle, \
	 "" using (1.32):(25):(sprintf('\\}')) with labels left font "Arial, 64"  notitle, \
	 "" using (1.46):(24.25):(sprintf('pread')) with labels left font "Arial Bold, 20"  notitle, \
	 "" using (fx(1,-0.10-0.025)-0.03):(25 + 2):(sprintf('4 KiB')) with labels left font "Arial Bold, 20" notitle, \
	 "<(sed -n '2p' results_0_small_16/pread_X_0_8192.dat)" using (fx(1,-0.00-0.025)):(22):(0):(fy($2/100)-22) with vectors filled head lw 3 lc "black" notitle, \
	 "" using (fx(1,-0.00-0.025)-0.03):(22 + 2):(sprintf('8 KiB')) with labels left font "Arial Bold, 20" notitle, \
	 "<(sed -n '2p' results_0_small_16/pread_X_0_16384.dat)" using (fx(1,+0.10-0.025)):(19):(0):(fy($2/100)-19) with vectors filled head lw 3 lc "black" notitle, \
	 "" using (fx(1,+0.10-0.025)-0.03):(19 + 2):(sprintf('16 KiB')) with labels left font "Arial Bold, 20" notitle, \
	 "<(sed -n '3p' results_0_small_16/pread_O_0_4096.dat)" using (2+0.20-0.025):(25):(0):(($2/100)-25) with vectors filled head lw 3 lc "black" notitle, \
	 "" using (2.685):(25):(sprintf('\\}')) with labels left font "Arial, 64"  notitle, \
	 "" using (2.825):(25.75):(sprintf('pread\n+ Direct I/O')) with labels left font "Arial Bold, 20" notitle, \
	 "" using (2+0.20-0.025-0.03):(25 + 2):(sprintf('4 KiB')) with labels left font "Arial Bold, 20" notitle, \
	 "<(sed -n '3p' results_0_small_16/pread_O_0_8192.dat)" using (2+0.30-0.025):(22):(0):(($2/100)-22) with vectors filled head lw 3 lc "black" notitle, \
	 "" using (2+0.30-0.025-0.03):(22 + 2):(sprintf('8 KiB')) with labels left font "Arial Bold, 20" notitle, \
	 "<(sed -n '3p' results_0_small_16/pread_O_0_16384.dat)" using (2+0.40-0.025):(19):(0):(($2/100)-19) with vectors filled head lw 3 lc "black" notitle, \
	 "" using (2+0.40-0.025-0.03):(19 + 2):(sprintf('16 KiB')) with labels left font "Arial Bold, 20" notitle

set border

set tmargin at screen 1. - 0.55
set bmargin at screen 1. - 0.88

set yrange [0:22]
set ytics 4,4,16 font ", 20" scale 0.0

set xtics font ", 20" scale 0.0
set xlabel 'Number of Clients' font "Arial, 22" offset 0, -0.4
set y2label "256 GiB Memory" font "Arial Bold, 20" offset 0.3, -0.75

plot "results_0_large_256/zicio_X_0.dat" using ($0-0.30):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
	 "results_0_large_256/zicio_X_0.dat" using ($0-0.30):($5/100) with boxes fs solid lt rgb "red" notitle, \
	 "results_0_large_256/zicio_X_0.dat" using ($0-0.30):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle, \
	 \
	 "results_0_large_256/zicio_O_0.dat" using ($0-0.20):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
	 "results_0_large_256/zicio_O_0.dat" using ($0-0.20):($5/100) with boxes fs solid lt rgb "red" notitle, \
	 "results_0_large_256/zicio_O_0.dat" using ($0-0.20):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle, \
	 \
	 "results_0_large_256/pread_X_0_4096.dat" using ($0-0.10):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
	 "results_0_large_256/pread_X_0_4096.dat" using ($0-0.10):($5/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_large_256/pread_X_0_4096.dat" using ($0-0.10):($5/100) with boxes fs pattern 7 transparent lt rgb "black" notitle, \
	 "results_0_large_256/pread_X_0_4096.dat" using ($0-0.10):(($7+$8+$9)/100) with boxes fs solid lt rgb "#666666" notitle, \
	 "results_0_large_256/pread_X_0_4096.dat" using ($0-0.10):(($8+$9)/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_large_256/pread_X_0_4096.dat" using ($0-0.10):(($8+$9)/100) with boxes fs pattern 14 transparent lt rgb "black" notitle, \
	 "results_0_large_256/pread_X_0_4096.dat" using ($0-0.10):(($9)/100) with boxes fs solid lt rgb "#000000" notitle, \
	 "results_0_large_256/pread_X_0_4096.dat" using ($0-0.10):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle, \
	 \
	 "results_0_large_256/pread_X_0_8192.dat" using ($0-0.00):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
	 "results_0_large_256/pread_X_0_8192.dat" using ($0-0.00):($5/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_large_256/pread_X_0_8192.dat" using ($0-0.00):($5/100) with boxes fs pattern 7 transparent lt rgb "black" notitle, \
	 "results_0_large_256/pread_X_0_8192.dat" using ($0-0.00):(($7+$8+$9)/100) with boxes fs solid lt rgb "#666666" notitle, \
	 "results_0_large_256/pread_X_0_8192.dat" using ($0-0.00):(($8+$9)/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_large_256/pread_X_0_8192.dat" using ($0-0.00):(($8+$9)/100) with boxes fs pattern 14 transparent lt rgb "black" notitle, \
	 "results_0_large_256/pread_X_0_8192.dat" using ($0-0.00):(($9)/100) with boxes fs solid lt rgb "#000000" notitle, \
	 "results_0_large_256/pread_X_0_8192.dat" using ($0-0.00):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle, \
	 \
	 "results_0_large_256/pread_X_0_16384.dat" using ($0+0.10):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
	 "results_0_large_256/pread_X_0_16384.dat" using ($0+0.10):($5/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_large_256/pread_X_0_16384.dat" using ($0+0.10):($5/100) with boxes fs pattern 7 transparent lt rgb "black" notitle, \
	 "results_0_large_256/pread_X_0_16384.dat" using ($0+0.10):(($7+$8+$9)/100) with boxes fs solid lt rgb "#666666" notitle, \
	 "results_0_large_256/pread_X_0_16384.dat" using ($0+0.10):(($8+$9)/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_large_256/pread_X_0_16384.dat" using ($0+0.10):(($8+$9)/100) with boxes fs pattern 14 transparent lt rgb "black" notitle, \
	 "results_0_large_256/pread_X_0_16384.dat" using ($0+0.10):(($9)/100) with boxes fs solid lt rgb "#000000" notitle, \
	 "results_0_large_256/pread_X_0_16384.dat" using ($0+0.10):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle, \
	 \
	 "results_0_large_256/pread_O_0_4096.dat" using ($0+0.20):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
	 "results_0_large_256/pread_O_0_4096.dat" using ($0+0.20):($5/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_large_256/pread_O_0_4096.dat" using ($0+0.20):($5/100) with boxes fs pattern 7 transparent lt rgb "black" notitle, \
	 "results_0_large_256/pread_O_0_4096.dat" using ($0+0.20):(($7+$8+$9)/100) with boxes fs solid lt rgb "#666666" notitle, \
	 "results_0_large_256/pread_O_0_4096.dat" using ($0+0.20):(($8+$9)/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_large_256/pread_O_0_4096.dat" using ($0+0.20):(($8+$9)/100) with boxes fs pattern 14 transparent lt rgb "black" notitle, \
	 "results_0_large_256/pread_O_0_4096.dat" using ($0+0.20):(($9)/100) with boxes fs solid lt rgb "#000000" notitle, \
	 "results_0_large_256/pread_O_0_4096.dat" using ($0+0.20):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle, \
	 \
	 "results_0_large_256/pread_O_0_8192.dat" using ($0+0.30):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
	 "results_0_large_256/pread_O_0_8192.dat" using ($0+0.30):($5/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_large_256/pread_O_0_8192.dat" using ($0+0.30):($5/100) with boxes fs pattern 7 transparent lt rgb "black" notitle, \
	 "results_0_large_256/pread_O_0_8192.dat" using ($0+0.30):(($7+$8+$9)/100) with boxes fs solid lt rgb "#666666" notitle, \
	 "results_0_large_256/pread_O_0_8192.dat" using ($0+0.30):(($8+$9)/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_large_256/pread_O_0_8192.dat" using ($0+0.30):(($8+$9)/100) with boxes fs pattern 14 transparent lt rgb "black" notitle, \
	 "results_0_large_256/pread_O_0_8192.dat" using ($0+0.30):(($9)/100) with boxes fs solid lt rgb "#000000" notitle, \
	 "results_0_large_256/pread_O_0_8192.dat" using ($0+0.30):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle, \
	 \
	 "results_0_large_256/pread_O_0_16384.dat" using ($0+0.40):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
	 "results_0_large_256/pread_O_0_16384.dat" using ($0+0.40):($5/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_large_256/pread_O_0_16384.dat" using ($0+0.40):($5/100) with boxes fs pattern 7 transparent lt rgb "black" notitle, \
	 "results_0_large_256/pread_O_0_16384.dat" using ($0+0.40):(($7+$8+$9)/100) with boxes fs solid lt rgb "#666666" notitle, \
	 "results_0_large_256/pread_O_0_16384.dat" using ($0+0.40):(($8+$9)/100) with boxes fs solid lt rgb "#cccccc" notitle, \
	 "results_0_large_256/pread_O_0_16384.dat" using ($0+0.40):(($8+$9)/100) with boxes fs pattern 14 transparent lt rgb "black" notitle, \
	 "results_0_large_256/pread_O_0_16384.dat" using ($0+0.40):(($9)/100) with boxes fs solid lt rgb "#000000" notitle, \
	 "results_0_large_256/pread_O_0_16384.dat" using ($0+0.40):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle, \
	 \
	 "" using ($0):(0):xtic(sprintf("%d", $1)) with boxes notitle

unset xlabel
unset y2label
unset xtics

set tmargin at screen 1. - 0.62
set bmargin at screen 1. - 0.82
do for [pos=0:3] {
	set yrange[0:2.2]
		
	set lmargin at screen f(pos-0.35)
	set rmargin at screen f(pos+0.15)-0.01

	unset grid
	unset ytics
	set object rect from screen f(pos-0.35), 1.-0.82 to screen f(pos+0.15)-0.01, 1.-0.62 behind fillcolor rgb 'white' fillstyle solid noborder
	plot NaN notitle
	unset object

	set grid y
	set xrange [pos-0.4:pos+0.4-0.2]
	set ytics 1,1,3 font ", 18" scale 0.0

	plot "<(sed -n '".(pos+1)."p' results_0_large_256/zicio_X_0.dat)" using (pos-0.30):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/zicio_X_0.dat)" using (pos-0.30):($5/100) with boxes fs solid lt rgb "red" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/zicio_X_0.dat)" using (pos-0.30):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle, \
		 \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/zicio_O_0.dat)" using (pos-0.20):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/zicio_O_0.dat)" using (pos-0.20):($5/100) with boxes fs solid lt rgb "red" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/zicio_O_0.dat)" using (pos-0.20):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle, \
		 \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_4096.dat)" using (pos-0.10):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_4096.dat)" using (pos-0.10):($5/100) with boxes fs solid lt rgb "#cccccc" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_4096.dat)" using (pos-0.10):($5/100) with boxes fs pattern 7 transparent lt rgb "black" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_4096.dat)" using (pos-0.10):(($7+$8+$9)/100) with boxes fs solid lt rgb "#666666" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_4096.dat)" using (pos-0.10):(($8+$9)/100) with boxes fs solid lt rgb "#cccccc" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_4096.dat)" using (pos-0.10):(($8+$9)/100) with boxes fs pattern 14 transparent lt rgb "black" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_4096.dat)" using (pos-0.10):(($9)/100) with boxes fs solid lt rgb "#000000" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_4096.dat)" using (pos-0.10):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle, \
		 \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_8192.dat)" using (pos-0.00):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_8192.dat)" using (pos-0.00):($5/100) with boxes fs solid lt rgb "#cccccc" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_8192.dat)" using (pos-0.00):($5/100) with boxes fs pattern 7 transparent lt rgb "black" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_8192.dat)" using (pos-0.00):(($7+$8+$9)/100) with boxes fs solid lt rgb "#666666" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_8192.dat)" using (pos-0.00):(($8+$9)/100) with boxes fs solid lt rgb "#cccccc" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_8192.dat)" using (pos-0.00):(($8+$9)/100) with boxes fs pattern 14 transparent lt rgb "black" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_8192.dat)" using (pos-0.00):(($9)/100) with boxes fs solid lt rgb "#000000" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_8192.dat)" using (pos-0.00):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle, \
		 \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_16384.dat)" using (pos+0.10):($2/100) with boxes fs solid lt rgb "#dddddd" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_16384.dat)" using (pos+0.10):($5/100) with boxes fs solid lt rgb "#cccccc" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_16384.dat)" using (pos+0.10):($5/100) with boxes fs pattern 7 transparent lt rgb "black" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_16384.dat)" using (pos+0.10):(($7+$8+$9)/100) with boxes fs solid lt rgb "#666666" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_16384.dat)" using (pos+0.10):(($8+$9)/100) with boxes fs solid lt rgb "#cccccc" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_16384.dat)" using (pos+0.10):(($8+$9)/100) with boxes fs pattern 14 transparent lt rgb "black" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_16384.dat)" using (pos+0.10):(($9)/100) with boxes fs solid lt rgb "#000000" notitle, \
		 "<(sed -n '".(pos+1)."p' results_0_large_256/pread_X_0_16384.dat)" using (pos+0.10):($2/100):($3/100):($4/100) with yerrorbars ls 1 pt -1 ps 0 notitle
}

set lmargin at screen 0
set rmargin at screen 1
set tmargin at screen 1
set bmargin at screen 0

set xrange [0:8]
set yrange [0.01:4.5]
unset border

unset logscale y

unset xtics
unset xlabel
unset grid

set key reverse Left outside horizontal font ", 22" samplen 3 width 5
set key at 3.34, 4.47
plot "" using (0):(0) with boxes fs solid lt rgb "#dddddd" title "Data Ingestion"
set key at 5.99, 4.47
plot "" using (0):(0) with boxes fs solid lt rgb "#cccccc" title "Mode Switch ({/Times-New-Roman-Bold L2})"
plot "" using (0):(0) with boxes fs pattern 7 transparent lt rgb "black" title "                "
set key at 8.48, 4.47
plot "" using (0):(0) with boxes fs solid lt rgb "#666666" title "Data Copy ({/Times-New-Roman-Bold L3})    "
plot "" using (0):(0) with boxes fs pattern 8 transparent lt rgb "black" title "                  "
set key at 3.8, 4.25
plot "" using (0):(0) with boxes fs solid lt rgb "#cccccc" title "Storage Stack ({/Times-New-Roman-Bold L4})"
plot "" using (0):(0) with boxes fs pattern 14 transparent lt rgb "black" title "                  "
set key at 6.1, 4.25
plot "" using (0):(0) with boxes fs solid lt rgb "#000000" title "Physical I/O ({/Times-New-Roman-Bold L5})"
set key at 8.6, 4.25
plot "" using (0):(0) with boxes fs solid lt rgb "red" title "Wait Time in {/Times-New-Roman-Bold=22 U{/=18 ZIC}IO}"
plot "" using (0):(0) with boxes fs pattern 8 transparent lt rgb "black" title "                   "
unset key