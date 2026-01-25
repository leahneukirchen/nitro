require './t/case'

with_fixture "sv_a/run!" => <<EOF_A, "sv_a/finish!" => <<EOF_B do |svdir|
#!/bin/sh
exec sleep 3
EOF_A
#!/bin/sh
nitroctl down .
EOF_B
  testcase(svdir) { |events|
    # service is brought up by default
    events.poll_for(["UP", "sv_a"])
    match_seq?(events, [["STARTING", "sv_a"], ["UP", "sv_a"]])

    # service is not restarted
    events.poll_for(["DOWN", "sv_a"])
    match_seq?(events, [["RESTART", "sv_a"], ["SHUTDOWN", "sv_a"], ["DOWN", "sv_a"]])

    events.clear

    # service can be restarted manually
    `nitroctl restart sv_a`
    events.poll_for(["UP", "sv_a"])
  }
end
