require './t/case'

with_fixture "sv_a/run!" => <<EOF_A,
#!/bin/sh
exec sleep 100
EOF_A
             "sv_c/run!" => "", "sv_c/down" => "" do |svdir|
  testcase(svdir) { |events|
    events.poll_for(["STARTING", "sv_a"])

    pid = `nitroctl`[/sv_a.*pid (\d+)/, 1]
    pid.nil?  and raise "pid wrong"

    pid1 = `nitroctl pidof sv_a`
    pid == pid1.chomp  or raise "pid1 wrong"

    pid2 = `nitroctl pidof ../../../../../../../../../../../#{svdir}/sv_a`
    pid == pid2.chomp  or raise "pid2 wrong"

    pid3 = `nitroctl pidof #{svdir}/sv_a`
    pid == pid3.chomp  or raise "pid3 wrong"

    pid4 = `nitroctl pidof #{svdir}///sv_a/.`
    pid == pid4.chomp  or raise "pid4 wrong"
    
    pid5 = Dir.chdir(File.join(svdir, "sv_a")) {
      p Dir.pwd
      `nitroctl pidof .`
    }      
    pid == pid5.chomp  or raise "pid5 wrong"

    pid6 = Dir.chdir(File.join(svdir, "sv_a")) {
      `nitroctl pidof ../sv_a`
    }      
    pid == pid6.chomp  or raise "pid6 wrong"
  }
end
