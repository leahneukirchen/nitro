require './t/case'

with_fixture "sv_a/run!" => <<EOF_A, "sv_b/run!" => <<EOF_B,
#!/bin/sh
exec sleep 100
EOF_A
#!/bin/sh
exec sleep 100
EOF_B
             "SYS/setup!" => <<EOF_SYS_SETUP do |svdir|
#!/bin/sh
nitroctl down sv_a
EOF_SYS_SETUP
  testcase(svdir) { |events|
    events.poll_for(["UP", "sv_b"])

    match_seq?(events, [["SETUP", "SYS"],
                        ["DOWN", "sv_a"]])

    match_seq?(events, [["DOWN", "SYS"],
                        ["UP", "sv_b"]])

    `nitroctl list` =~ /DOWN sv_a/  or raise "sv_a not down"
  }
end
