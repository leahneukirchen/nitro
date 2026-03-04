require './t/case'

with_fixture "sv/run!" => <<EOF_A, "sv/down" => "", "sv/log=" => "../log", "log/run!" => <<EOF_B, "log/down" => "" do |svdir|
#!/bin/sh
echo 1
echo 2
echo 3
exec sleep 100
EOF_A
#!/bin/sh
pwd
exec cat > log.txt
EOF_B
  testcase(svdir) { |events|
    sleep 1
    `nitroctl` =~ /DOWN log/  or raise "log UP too soon"

    `nitroctl up sv`
    events.poll_for(["UP", "sv"])
    events.poll_for(["UP", "log"])

    puts `nitroctl`
    File.read(File.join(svdir, "log", "log.txt")) == "1\n2\n3\n"  or raise "log.txt wrong"
  }
end

