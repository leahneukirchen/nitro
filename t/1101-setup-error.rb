require './t/case'

with_fixture "sv_a/run!" => <<EOF_A, "sv_a/setup!" => <<EOF_SETUP do |svdir|
#!/bin/sh
exec sleep 100
EOF_A
#!/bin/sh
exit 2
EOF_SETUP
  testcase(svdir) { |events|
    sleep 5
    events.poll_for(["SETUP", "sv_a"])

    # process is stuck in state SETUP
    match_seq?(events, [["SETUP", "sv_a"], ["SETUP", "sv_a"], ["SETUP", "sv_a"]])
  }
end

