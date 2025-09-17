require './t/case'

with_fixture "sv_a/run!" => <<EOF_A, "sv_b/run!" => <<EOF_B do |svdir|
#!/bin/sh
exec sleep 100
EOF_A
#!/bin/sh
exec sleep 100
EOF_B
  testcase(svdir) { |events|
    # service is brought up by default
    events.poll_for(["UP", "sv_a"])
    events.poll_for(["UP", "sv_b"])
    match_seq?(events, [["STARTING", "sv_a"], ["UP", "sv_a"]])
    match_seq?(events, [["STARTING", "sv_b"], ["UP", "sv_b"]])

    # services get brought down on shutdown
    `nitroctl Shutdown`
    events.poll_for(["DOWN", "sv_a"])
    events.poll_for(["DOWN", "sv_b"])
    match_seq?(events, [["UP", "sv_a"], ["DOWN", "sv_a"]])
    match_seq?(events, [["UP", "sv_b"], ["DOWN", "sv_b"]])
  }
end

