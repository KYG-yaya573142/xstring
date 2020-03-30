reset
set xlabel 'string length (bytes)'
set ylabel 'time (ns)'
set title 'xs\_cpy runtime'
set term png enhanced font 'Verdana,10'
set output 'plot_output.png'
set grid
plot [1:][0:300] \
'out' using 1:2 with linespoints linewidth 2 title "w/ CoW",\
'out_old' using 1:2 with linespoints linewidth 2 title "w/o CoW"