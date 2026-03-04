require './t/case'

with_fixture "sv@/run!" => <<EOF_A, "LOG@/run!" => <<EOF_B do |svdir|
#!/bin/sh
echo $1 1
echo $1 2
echo $2 3
exec sleep 100
EOF_A
#!/bin/sh
echo $1
pwd
exec cat > log.$1
EOF_B
  testcase(svdir) { |events|
    sleep 1
    `nitroctl up sv@one`
    `nitroctl up sv@two`
    events.poll_for(["UP", "sv@one"])
    events.poll_for(["UP", "sv@two"])

    events.poll_for(["UP", "LOG@one"])
    events.poll_for(["UP", "LOG@two"])

    `nitroctl down sv@two LOG@two`
    `nitroctl rescan`
    `nitroctl rescan`

    `nitroctl` =~ /two/ and raise "sv@two was not cleaned up completely"

    File.read(File.join(svdir, "LOG@", "log.one")) =~ /one 2/  or raise "LOG@one wrong"
    File.read(File.join(svdir, "LOG@", "log.two")) =~ /two 2/  or raise "LOG@two wrong"
  }
end

