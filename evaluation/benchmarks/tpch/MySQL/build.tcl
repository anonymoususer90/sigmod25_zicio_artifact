#!/bin/tclsh
puts "build the schema of TPC-H"
global complete
proc wait_to_complete {} {
  global complete
  set complete [vucomplete]
  if {!$complete} { after 5000 wait_to_complete } else { exit }
}

set thisFile [ dict get [ info frame 0 ] file ]

set BUILD_CONFIG [open "../config.txt"]
set CONFIG_LINES [split [read $BUILD_CONFIG] "\n"]
close $BUILD_CONFIG

# set port number
set PORT [lindex $CONFIG_LINES 0]
# set scale factor
set SCALE_FACTOR [lindex $CONFIG_LINES 1]
# set thread num
set THREAD_NUM [lindex $CONFIG_LINES 2]
# socket
set SOCKET [lindex $CONFIG_LINES 3]

puts "set database MySQL"
dbset db mysql
puts "set target TPC-H"
dbset bm TPC-H

diset connection mysql_host localhost
diset connection mysql_port $PORT
diset connection mysql_socket $SOCKET
diset tpch mysql_tpch_user tpch
diset tpch mysql_tpch_pass tpch
diset tpch mysql_tpch_dbase tpch
diset tpch mysql_tpch_storage_engine myisam

diset tpch mysql_num_tpch_threads $THREAD_NUM
diset tpch mysql_scale_fact $SCALE_FACTOR

print dict
loadscript

vuset delay 10

buildschema
wait_to_complete
vwait forever
