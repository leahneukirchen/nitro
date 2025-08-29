require './t/case'

with_fixture "sv_a/run!" => <<EOF_A, "sv_a/setup!" => <<EOF_SETUP do |svdir|
#!/bin/sh
sleep 100
EOF_A
#!/bin/sh
true
EOF_SETUP
  testcase(svdir) { |events|
    events.poll_for(["UP", "sv_a"])
    
    match_seq?(events, [["SETUP", "sv_a"], ["STARTING", "sv_a"], ["UP", "sv_a"]])
  }
end

