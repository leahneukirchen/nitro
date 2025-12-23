require './t/case'

with_fixture "sv@/run!" => <<EOF_A, "sv@/log=" => "../mylog@", "mylog@/run!" => <<EOF_B do |svdir|
#!/bin/sh
echo 1
echo 2
echo 3
exec sleep 100
EOF_A
#!/bin/sh
echo $1
exec cat
EOF_B
  testcase(svdir) { |events|
    sleep 1
    `nitroctl up sv@one`
    `nitroctl up sv@two`
    events.poll_for(["UP", "sv@one"])
    events.poll_for(["UP", "sv@two"])

    events.poll_for(["UP", "mylog@one"])
    events.poll_for(["UP", "mylog@two"])

    `nitroctl down sv@two mylog@two`
    `nitroctl rescan`
    `nitroctl rescan`

    `nitroctl` =~ /two/ and raise "sv@two was not cleaned up completely"
  }
end

