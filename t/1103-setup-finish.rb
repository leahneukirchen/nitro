require './t/case'

with_fixture "sv_a/run!" => <<EOF_A, "sv_a/setup!" => <<EOF_SETUP, "sv_a/finish!" => <<EOF_FINISH do |svdir|
#!/bin/sh
sleep 100
EOF_A
#!/bin/sh
true
EOF_SETUP
#!/bin/sh
true
EOF_FINISH
  testcase(svdir) { |events|
    events.poll_for(["UP", "sv_a"])
    `nitroctl down sv_a`

    events.poll_for(["DOWN", "sv_a"])
    p events
    
    match_seq?(events, [["SETUP", "sv_a"], ["STARTING", "sv_a"], ["UP", "sv_a"], ["SHUTDOWN", "sv_a"], ["DOWN", "sv_a"]])
  }
end

