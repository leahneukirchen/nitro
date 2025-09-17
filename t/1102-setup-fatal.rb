require './t/case'

with_fixture "sv_a/run!" => <<EOF_A, "sv_a/setup!" => <<EOF_SETUP do |svdir|
#!/bin/sh
exec sleep 100
EOF_A
#!/bin/sh
exit 111
EOF_SETUP
  testcase(svdir) { |events|
    events.poll_for(["FATAL", "sv_a"])

    # process is marked FATAL
    match_seq?(events, [["SETUP", "sv_a"], ["FATAL", "sv_a"]])
  }
end
