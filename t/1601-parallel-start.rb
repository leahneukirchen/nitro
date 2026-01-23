require './t/case'

with_fixture "sv_a/run!" => <<EOF_A, "sv_b/run!" => <<EOF_B, "sv_c/run!" => <<EOF_C,
#!/bin/sh
exec sleep 100
EOF_A
#!/bin/sh
exec sleep 100
EOF_B
#!/bin/sh
exec sleep 100
EOF_C
             "sv_a/down" => "", "sv_b/down" => "", "sv_c/down" => "" do |svdir|
  testcase(svdir) { |events|
    sleep 0.5
    `nitroctl -t 3 start sv_a sv_b sv_c`
    $?.exitstatus == 0  or raise "start failed"

    events.poll_for(["UP", "sv_a"])
    events.poll_for(["UP", "sv_b"])
    events.poll_for(["UP", "sv_c"])

    match_seq?(events, [["STARTING", "sv_b"],
                        ["UP", "sv_a"]])
    match_seq?(events, [["STARTING", "sv_c"],
                        ["UP", "sv_a"]])

    match_seq?(events, [["STARTING", "sv_a"],
                        ["UP", "sv_b"]])
    match_seq?(events, [["STARTING", "sv_c"],
                        ["UP", "sv_b"]])
  }
end
