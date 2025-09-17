require './t/case'

with_fixture "sv@/run!" => <<EOF_A do |svdir|
#!/bin/sh
exec sleep 100
EOF_A
  testcase(svdir) { |events|
    sleep 1
    `nitroctl`.empty?  or raise

    `nitroctl up sv@one`
    events.poll_for(["STARTING", "sv@one"])

    `nitroctl up sv@two`
    events.poll_for(["UP", "sv@two"])

    `nitroctl restart sv@four`
    events.poll_for(["UP", "sv@four"])

    match_seq?(events, [["STARTING", "sv@one"], ["UP", "sv@one"]])
    match_seq?(events, [["STARTING", "sv@two"], ["UP", "sv@two"]])
    match_seq?(events, [["STARTING", "sv@four"], ["UP", "sv@four"]])

    `nitroctl rescan`
    `nitroctl` =~ /UP sv@one/  or raise
    `nitroctl` =~ /UP sv@two/  or raise

    `mkdir #{svdir}/sv@three`
    `nitroctl rescan`
    events.poll_for(["STARTING", "sv@three"])

    `nitroctl down sv@one`
    events.poll_for(["DOWN", "sv@one"])

    `nitroctl down sv@three`
    events.poll_for(["DOWN", "sv@one"])
    `nitroctl rescan`
    events.poll_for(["DOWN", "sv@one"])
    events.poll_for(["DOWN", "sv@three"])

    `nitroctl` =~ /sv@one/  and raise "sv@one was not zapped"
    `nitroctl` =~ /DOWN sv@three/  or raise

    `mv #{svdir}/sv@ #{svdir}/svx@`

    `nitroctl rescan`
    events.poll_for(["DOWN", "sv@two"])
    
    # service stays in list as it has a filesystem reference
    `nitroctl` =~ /DOWN sv@three/  or raise
  }
end

