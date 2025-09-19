require './t/case'

with_fixture "sv_a/run!" => <<EOF_A, "sv_b/run!" => <<EOF_B,
#!/bin/sh
exec sleep 100
EOF_A
#!/bin/sh
exec sleep 100
EOF_B
             "SYS/setup!" => <<EOF_SYS_SETUP, "SYS/finish!" => <<EOF_SYS_FINISH do |svdir|
#!/bin/sh
nitroctl start sv_a
EOF_SYS_SETUP
#!/bin/sh
nitroctl restart sv_b
nitroctl stop sv_b
EOF_SYS_FINISH
  testcase(svdir) { |events|
    events.poll_for(["UP", "sv_b"])

    match_seq?(events, [["SETUP", "SYS"],
                        ["STARTING", "sv_a"], ["UP", "sv_a"],
                        ["DOWN", "SYS"],
                        ["STARTING", "sv_b"], ["UP", "sv_b"]])
    `nitroctl Shutdown`

    events.poll_for(["DOWN", "sv_a"])

    match_seq?(events, [["DOWN", "SYS"],
                        ["UP", "sv_b"],
                        ["DOWN", "sv_b"],
                        ["DOWN", "sv_a"]])
  }
end
