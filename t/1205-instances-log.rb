require './t/case'

with_fixture "sv_a/run!" => <<EOF_A, "sv_a/log=" => "../mylog@", "sv_b/run!" => <<EOF_B, "sv_b/log=" => "../mylog@", "mylog@/run!" => <<EOF_C do |svdir|
#!/bin/sh
echo 1
echo 2
echo 3
exec sleep 100
EOF_A
#!/bin/sh
echo 4
echo 5
echo 6
exec sleep 100
EOF_B
#!/bin/sh
echo $1
exec cat
EOF_C
  testcase(svdir) { |events|
    events.poll_for(["UP", "sv_a"])
    events.poll_for(["UP", "sv_b"])

    events.poll_for(["UP", "mylog@sv_a"])
    events.poll_for(["UP", "mylog@sv_b"])
  }
end

