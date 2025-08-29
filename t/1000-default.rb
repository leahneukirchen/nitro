require './t/case'

with_fixture "sv_a/run!" => <<EOF do |svdir|
#!/bin/sh
sleep 100
EOF
  testcase(svdir) { |events|
    # service is brought up by default
    events.poll_for(["UP", "sv_a"])
    match_seq?(events, [["STARTING", "sv_a"], ["UP", "sv_a"]])

    # services get brought down on shutdown
    `nitroctl Shutdown`
    events.poll_for(["DOWN", "sv_a"])
    match_seq?(events, [["UP", "sv_a"], ["DOWN", "sv_a"]])
  }
end
