require './t/case'

with_fixture "sv_a/run!" => <<EOF_A, "sv_a/log=" => "../sv_a" do |svdir|
#!/bin/sh
echo 111
echo 222
echo 333
read line
[ "$line" != 111 ] && nitroctl down .
exec sleep 100
EOF_A
  testcase(svdir) { |events|
    events.poll_for(["UP", "sv_a"])
  }
end
