require './t/case'

with_fixture "sv_a/run!" => <<EOF_A, "sv_a/log=" => "../mylog", "mylog/run!" => <<EOF_B do |svdir|
#!/bin/sh
ruby -e 'exit STDOUT.stat.pipe?'
echo $? >checkstdout
echo 1
sleep 1
echo 2
echo 3
sleep 100
EOF_A
#!/bin/sh
ruby -e 'exit STDIN.stat.pipe?'
echo $? >checkstdin
exec cat >mylog.txt
EOF_B
  testcase(svdir) { |events|
    events.poll_for(["STARTING", "sv_a"])
    events.poll_for(["UP", "mylog"])

    File.read(File.join(svdir, "sv_a/checkstdout")) == "0\n"  or raise "no output pipe created"
    File.read(File.join(svdir, "mylog/checkstdin")) == "0\n"  or raise "no input pipe created"

    File.read(File.join(svdir, "mylog/mylog.txt")) == "1\n2\n3\n"  or raise "wrong log 1"

    `nitroctl restart sv_a`

    File.read(File.join(svdir, "mylog/mylog.txt")) == "1\n2\n3\n1\n2\n3\n"  or raise "wrong log 2"

    `nitroctl fast-restart mylog`
    `nitroctl restart sv_a`

    File.read(File.join(svdir, "mylog/mylog.txt")) == "1\n2\n3\n"  or raise "wrong log 3"
  }
end
