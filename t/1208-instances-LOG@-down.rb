require './t/case'

with_fixture "sv_a/run!" => <<EOF_A, "sv_a/down" => "", "LOG@/run!" => <<EOF_B do |svdir|
#!/bin/sh
echo 1
echo 2
echo 3
exec sleep 100
EOF_A
#!/bin/sh
echo $1
pwd
exec cat > log.txt
EOF_B
  testcase(svdir) { |events|
    sleep 1
    `nitroctl` =~ /DOWN LOG@sv_a/  or raise "logger was started unnecessarily"
  }
end

