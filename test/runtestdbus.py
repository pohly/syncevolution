#! /usr/bin/python

'''
 Copyright (C) 2010 Intel Corporation

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) version 3.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.

 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 02110-1301  USA
'''

#-*-coding:utf-8-*-
'''
runtestdbus.py: configure testing environment, get source code and compile
                run test-dbus.py to do dbus testing and collect results
                publish results to Internet and send to related recipients
'''
import sys
import os
import time,datetime
import email
import mimetypes
from email.MIMEMultipart import MIMEMultipart
from email.MIMEText import MIMEText
from email.MIMEImage import MIMEImage
import smtplib
from xml.dom import minidom,Node
import unittest
import shutil
import subprocess
import getopt
import popen2

gitServer_libsynthesis="git://gitorious.org/meego-middleware/libsynthesis.git"
gitServer_syncevolution="git://gitorious.org/meego-middleware/syncevolution.git"

USAGE='''\
Usage: python runtestdbus.py [options] [testName]

Selective Options:
    -h,-H,--help     Show this message
    --recompile=     recompile from the source or not
    -s,--show        Show all test cases
    --shell=         prefix of a command to be executed

Mandatory Options for configuring dbus testing:
    --binpath=       directory for installing syncevolution
    --from=          sender of result email 
    --mailhost=      mail host server for sending email
    --resultpath=    directory for saving the resultdirs
    --srcpath=       directory for saving source code
    --testpath=      directory for intermediate files during testing
    --to=            recipients of result email
    --workpath=      the main work directory
    --webpath=       directory for publishing results to the Internet
    --webserver=     web server address for publishing results

Examples:
    run all test cases: 
    rundbustest.py  --workpath=your_work_directory 
                    --srcpath=source_code_directory
                    --binpath=directory_for_installing
                    --resultpath=directory_for_saving_results
                    --webpath=directory_to_publish_results
                    --testpath=intermediate_directory
                    --webserver=http://www.example.com
                    --recompile=NO
                    --from=sender
                    --to=recipients
                    --mailhost=smtp.example.com 
'''
def abspath(path):
    """Absolute path after expanding vars and user."""
    return os.path.abspath(os.path.expanduser(os.path.expandvars(path)))

def runCommand(cmd):
        '''run the shell command'''
        flag=os.system(cmd)
        if flag!=0:
            raise Exception("%s: failed (return code %d)" % (cmd, flag>>8))

class GetSource():
    '''this class is for getting source from the server and compile them'''
    def make(self):
        f=open(today_result+"/compile_result.txt","w")
        self.makelibsynthesis(f)
        self.makesyncevolution(f)
        f.close()

    def makelibsynthesis(self,f):
        libsynthesisD=srcpath+"/libsynthesis"
        libsynthesisbin=binpath+"/libsynthesis"
        if os.path.exists(libsynthesisD):
            cmd='cd %s && git pull origin master > %s/fetchlibsynthesis.log 2>&1' % (libsynthesisD,today_result)
            pass
        else:
            cmd='cd %s && git clone %s > %s/fetchlibsynthesis.log 2>&1' % (srcpath,gitServer_libsynthesis,today_result)
        if os.system(cmd):
            f.write("FAIL")
        else:
            f.write("OK")
        f.write('\n')
        if not os.path.exists(libsynthesisbin):
            os.makedirs(libsynthesisbin)
        cmd='cd %s &&(%s sh autogen.sh) > %s/compile.log 2>&1' %(libsynthesisD,shell,today_result)
        if os.system(cmd):
            f.write("FAIL")
        else:
            f.write("OK")
        f.write('\n')

    def makesyncevolution(self,f):
        syncevolutionD=srcpath+"/syncevolution"
        syncevolutionbin=binpath+"/syncevolution"
        if os.path.exists(syncevolutionD):
            cmd='cd %s && git pull origin master > %s/fetchsyncevolution.log 2>&1' % (syncevolutionD,today_result)
            pass
        else:
            cmd='cd %s && git clone %s > %s/fetchsyncevolution.log 2>&1' % (srcpath,gitServer_syncevolution,today_result)
        if os.system(cmd):
            f.write("FAIL")
        else:
            f.write("OK")
        f.write('\n')
        if not os.path.exists(syncevolutionbin):
            os.makedirs(syncevolutionbin)
        cmd = 'cd %s && (%s sh autogen.sh && cd %s && %s %s/configure --with-synthesis-src=%s/libsynthesis \
                --prefix=%s --enable-dbus-service --enable-gui --enable-unit-tests && %s make -j 8 &&  \
                %s make install) >> %s/compile.log 2>&1' % (syncevolutionD, shell, syncevolutionbin, shell, \
                syncevolutionD, srcpath, binpath, shell, shell, today_result)
        if os.system(cmd):
            f.write("FAIL")
        else:
            f.write("OK")
        f.write('\n')


class GetTestcaseNames():
        '''this class can get all testcases from the specified module,before using
            it,we should copy the test/test-dbus.py to the current directory and 
            rename the test-dbus.py to testdbus'''
        testcaseNames=[]
        testcaseCounts=""
        
        def __init__(self):
            '''import module testdbus and copy the directory test/test-dbus in the path
               of workpath/syncevolution/ to the workpath'''
            srcfile=workpath+"/syncevolution/test/test-dbus.py"
            srcdirec=workpath+"/syncevolution/test/test-dbus"
            currentpath=os.getcwd()
            destfile=currentpath+"/testdbus.py"
            destdirec=currentpath+"/test/test-dbus"
            
            if os.path.isfile(destfile):
                os.system("rm -rf testdbus.py")
            shutil.copyfile(srcfile,destfile)
            import testdbus
            
            if os.path.exists(destdirec):
                shutil.rmtree(destdirec,True)
            shutil.copytree(srcdirec,destdirec)

	def normalNames(self,names):
	    '''transform testcase names into the "ClassName.testMethod" formation'''
	    i=0
	    length=len(names)
	    testcaseNames=[]
	    for i in range(length):
		testCaseName=names[i].split(' ')[0]
		className=names[i].split(' ')[1]
		className=className[1:len(className)-1]
		className=className.split('.')[1]
		testcaseNames.append(className+'.'+testCaseName)
	    return testcaseNames

	def gettestcaseName(self,module):
	    '''get the testcase names from the module'''
	    suites=unittest.TestLoader().loadTestsFromModule(module)
	    testcase_Counts=0
	    names=[]
	    for suite in suites:
		for item in suite:
		    testcase_Counts+=1
		    names.append(str(item))
            self.testcaseCounts=testcase_Counts
	    self.testcaseNames=self.normalNames(names)

        def writeToFile(self):
            '''write the testcase names into to the file "testCaseNames.txt"'''
            f=open(workpath+"/testCaseNames.txt","w")
            for item in self.testcaseNames:
                f.write(item)
                f.write('\n')
            f.close()


class RunTestcase():
    '''run the specefied testcase'''
    def __init__(self,singletestcase):
        self.singletestcase=singletestcase
        self.server=[workpath+"/syncevolution/test/test-dbus.py"]
        f=open(workpath+"/testCaseNames.txt","r")
        self.testcaseNames=f.readlines()
        f.close()
        cmd='echo $DBUS_SESSION_BUS_ADDRESS > %s/dbus_session_address' % workpath
        runCommand(cmd)

    def run(self):
        currentTime=time.strftime("%Y%m%d",time.localtime())
        cmd='mkdir -p %s/test-dbus-log;' % today_result
        runCommand(cmd)
        if self.singletestcase != None:
            testcaseName=[]
            for name in self.testcaseNames:
                if not name.find(self.singletestcase.strip()):
                    testcaseName.append(name) 
            self.testcaseNames=testcaseName
        for name in self.testcaseNames:
            cmd='rm -fr %s' % testpath
            runCommand(cmd)
            logname=today_result+"/test-dbus-log/"+name.rstrip()+".log"
            f=open(logname,"w")
            f.write("python "+self.server[0]+" "+name.strip())
            f.write('\n')
            f.flush()
            obj=popen2.Popen4("CLIENT_TEST_EVOLUTION_PREFIX=file://%s/ XDG_CONFIG_HOME=~/.config/ \
					           PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:%s/libexec/:%s/bin/ \
							   %s " % (testpath,binpath,binpath,shell) + self.server[0]+" "+name.strip())
            fout=obj.fromchild.readlines()
            for line in fout:
                f.write(line)
            obj.wait()
            f.close()
        


class GetResult():
    '''the result is like {"testcases":{className:{testcaseName:{},},},},and we can add what we like in the struct result'''
    def __init__(self,testcase,compile_status_flag):
        self.testcase=testcase
        self.compile_status_flag=compile_status_flag
        self.result={}
        yesterdayTime=''.join(str(datetime.datetime.now()+datetime.timedelta(days=-1)).split(' ')[0].split('-'))
        self.today_logPath=webdir+"/"+time.strftime("%Y%m%d",time.localtime())+"/test-dbus-log/"
        self.yesterday_logPath=webdir+"/"+yesterdayTime+"/test-dbus-log/"
        self.today_compile_log=webdir+"/"+time.strftime("%Y%m%d",time.localtime())+"/compile_result.txt"
        self.yesterday_compile_log=webdir+"/"+yesterdayTime+"/compile_result.txt"

    def getResult(self):
        '''the all results'''
        self.getplatformInfo()
        self.getprepareResult()
        if self.compile_status_flag == 'OK':
            self.gettestResult()

    def getprepareResult(self):
        '''get compile results'''
        self.result["prepareResult"]={"compile":{"today_status":"","yesterday_status":"","url":""},"dist":"skipped",\
                                      "libsysnthesis-fetch-config":{"status":"OK","yesterday_status":"","url":""},\
                                      "syncevolution-fetch-config":{"status":"OK","yesterday_status":"","url":""}}
        try:
            f1=open(self.today_compile_log,"r")            
            self.result["prepareResult"]["libsysnthesis-fetch-config"]["today_status"]=f1.readline().strip()            
            self.result["prepareResult"]["libsysnthesis-fetch-config"]["url"]=server+"/dbus_results/"+time.strftime("%Y%m%d",time.localtime())+"/fetchlibsynthesis.log"  
            self.result["prepareResult"]["compile"]["today_status"]=f1.readline().strip()
            self.result["prepareResult"]["syncevolution-fetch-config"]["today_status"]=f1.readline().strip()
            self.result["prepareResult"]["syncevolution-fetch-config"]["url"]=server+"/dbus_results/"+time.strftime("%Y%m%d",time.localtime())+"/fetchsyncevolution.log"
            if self.result["prepareResult"]["compile"]["today_status"] == "OK":
	            self.result["prepareResult"]["compile"]["today_status"]=f1.readline().strip()
            self.result["prepareResult"]["compile"]["url"]=server+"/dbus_results/"+time.strftime("%Y%m%d",time.localtime())+"/compile.log"
            f1.close()

            f2=open(self.yesterday_compile_log,"r")
            self.result["prepareResult"]["libsysnthesis-fetch-config"]["yesterday_status"]=f2.readline().strip()
            self.result["prepareResult"]["compile"]["yesterday_status"]=f2.readline().strip()
            self.result["prepareResult"]["syncevolution-fetch-config"]["yesterday_status"]=f2.readline().strip()
            self.result["prepareResult"]["compile"]["yesterday_status"]=f2.readline().strip()
            f2.close()
        except IOError,e:
            print str(e)
        
        

    def getplatformInfo(self):
        '''get system information'''
        self.result["platformInfo"]={"cpuinfo":"","memoryinfo":"","osinfo":"",\
                                    "libraryinfo":{"libsoup":"","evolution-data-server":"","glib":"","dbus-glib":""}}
        if os.path.isfile(os.getcwd()+"/platformInfo.txt"):
            cmd="rm %s/platformInfo.txt" %(workpath)
            runCommand(cmd)
        cmd="cat /proc/cpuinfo  | grep 'model name' | awk -F\: '{print $2}'|uniq|sed -e 's/ //' >>platformInfo.txt"
        runCommand(cmd)
        cmd="cat /proc/meminfo | grep MemTotal | awk -F\: '{print $2}' | awk -F\  '{print $1 " " $2}' >>platformInfo.txt"
        runCommand(cmd)
        cmd="cat /proc/meminfo | grep MemFree | awk -F\: '{print $2}' | awk -F\  '{print $1 " " $2}' >>platformInfo.txt"
        runCommand(cmd)
        cmd="cat /proc/meminfo | grep SwapTotal | awk -F\: '{print $2}' | awk -F\  '{print $1 " " $2}' >>platformInfo.txt"
        runCommand(cmd)
        cmd="uname -sr >>platformInfo.txt"
        runCommand(cmd)
        
        cmd="pkg-config --list-all | grep libsoup-[0-9] | awk '{print $1}' >libraryInfo.txt"
        runCommand(cmd)
        cmd='read libname <libraryInfo.txt && (echo $libname;pkg-config --modversion $libname) >>platformInfo.txt'
        runCommand(cmd)
        cmd="pkg-config --list-all | grep evolution-data-server | awk '{print $1}' >libraryInfo.txt"
        runCommand(cmd)
        cmd='read libname <libraryInfo.txt && (echo $libname;pkg-config --modversion $libname) >>platformInfo.txt'
        runCommand(cmd)
        cmd="pkg-config --list-all | grep ^glib-[0-9] | awk '{print $1}' >libraryInfo.txt"
        runCommand(cmd)
        cmd='read libname <libraryInfo.txt && (echo $libname;pkg-config --modversion $libname) >>platformInfo.txt'
        runCommand(cmd)
        cmd="pkg-config --list-all | grep ^dbus-glib-[0-9] | awk '{print $1}' >libraryInfo.txt"
        runCommand(cmd)
        cmd='read libname <libraryInfo.txt && (echo $libname;pkg-config --modversion $libname) >>platformInfo.txt'
        runCommand(cmd)
        cmd="rm -f libraryInfo.txt"
        runCommand(cmd)
        try:
            f=open(os.getcwd()+"/platformInfo.txt","r")
            cpuinfo="model name:"+f.readline()
            memoryinfo="MemTotal:"+f.readline().rstrip()+" MemFree:"+f.readline().rstrip()+" SwapTotal:"+f.readline()
            osinfo=f.readline()
            libsoup=f.readline().rstrip()+":"+f.readline().rstrip()
            libevolution=f.readline().rstrip()+":"+f.readline().rstrip()
            libglib=f.readline().rstrip()+":"+f.readline().rstrip()
            libdbusglib=f.readline().rstrip()+":"+f.readline().rstrip()
            self.result["platformInfo"]["cpuinfo"]=cpuinfo.rstrip()
            self.result["platformInfo"]["memoryinfo"]=memoryinfo.rstrip()
            self.result["platformInfo"]["osinfo"]=osinfo.rstrip()
            self.result["platformInfo"]["libraryinfo"]["libsoup"]=libsoup
            self.result["platformInfo"]["libraryinfo"]["evolution-data-server"]=libevolution
            self.result["platformInfo"]["libraryinfo"]["glib"]=libglib
            self.result["platformInfo"]["libraryinfo"]["dbus-glib"]=libdbusglib
            f.close()
        except IOError,e:
            print str(e)

    def gettestResult(self):
        '''the following code is setting the struct of result'''
        self.result["testcases"]={}
        try:
            Names=[]
            f=open(workpath+"/testCaseNames.txt","r")
            AllNames=f.readlines()
            f.close()
            if self.testcase != None:
                for name in AllNames:
                    if not name.find(self.testcase[0]):
                        Names.append(name)
            else:
                Names=AllNames
            for name in Names:
                className=name.split(".")[0]
                testcaseName=name.split(".")[1].rstrip()
                if self.result["testcases"].has_key(className):
                    self.result["testcases"][className][testcaseName]={"today_status":"","yesterday_status":"","url":""}
                else:
                    self.result["testcases"][className]={testcaseName:{"today_status":"","yesterday_status":"","url":""}}
        except IOError,e:
            print str(e)

        '''the following code is for setting item in result'''
        for className in self.result["testcases"]:
            for testcaseName in self.result["testcases"][className]:
                try:
                    logurl1=self.today_logPath+className+"."+testcaseName+".log"                    
                    f1=open(logurl1,"r")                    
                    f1.readline()                    
                    firstline1=f1.readline().rstrip()                    
                    self.result["testcases"][className][testcaseName]["url"]=logurl1.replace(webdir,server+"/dbus_results")
                    if not firstline1.find("."):
                        self.result["testcases"][className][testcaseName]["today_status"]="OK"
                    elif not firstline1.find("F"):
                        self.result["testcases"][className][testcaseName]["today_status"]="FAIL"
                    elif not firstline1.find("E"):
                        self.result["testcases"][className][testcaseName]["today_status"]="ERROR"
                    elif firstline1=="":
                        self.result["testcases"][className][testcaseName]["today_status"]="ERROR"
                    else:
                        self.result["testcases"][className][testcaseName]["today_status"]=firstline1

                    logurl2=self.yesterday_logPath+className+"."+testcaseName+".log"
                    f2=open(logurl2,"r")
                    f2.readline()
                    firstline2=f2.readline().rstrip()
                    if not firstline2.find("."):
                        self.result["testcases"][className][testcaseName]["yesterday_status"]="OK"
                    elif not firstline2.find("F"):
                        self.result["testcases"][className][testcaseName]["yesterday_status"]="FAIL"
                    elif not firstline2.find("E"):
                        self.result["testcases"][className][testcaseName]["yesterday_status"]="ERROR"
                    elif firstline2=="":
                        self.result["testcases"][className][testcaseName]["yesterday_status"]="ERROR"
                    else:
                        self.result["testcases"][className][testcaseName]["yesterday_status"]=firstline2
                    f1.close()
                    f2.close()
                except IOError,e:
                    print str(e)


class ResultToXML():
    '''this class is used to generate file of xml type from the dic result'''
    def __init__(self,result):
        self.result=result

    def generateXML(self):
        doc=minidom.Document()
        result=doc.createElement("result")
        doc.appendChild(result)

        for item1 in self.result:
            first=doc.createElement(item1)#testcases
            if str(type(self.result[item1])) == "<type 'dict'>":
                first.setAttribute("ChildNumber",str(len(self.result[item1].keys())))
                for item2 in self.result[item1]:
                    second=doc.createElement(item2)#className
                    if str(type(self.result[item1][item2])) == "<type 'dict'>":
                        second.setAttribute("ChildNumber",str(len(self.result[item1][item2].keys())))
                        for item3 in self.result[item1][item2]:
                            third=doc.createElement(item3)#testcaseName
                            if str(type(self.result[item1][item2][item3])) == "<type 'dict'>":
                                third.setAttribute("ChildNumber",str(len(self.result[item1][item2][item3].keys())))
                                for key in self.result[item1][item2][item3].keys():
                                    fourth=doc.createElement(key)
                                    text=doc.createTextNode(self.result[item1][item2][item3][key])
                                    fourth.appendChild(text)
                                    third.appendChild(fourth)
                            else:
                                text=doc.createTextNode(self.result[item1][item2][item3])
                                third.appendChild(text)
                            second.appendChild(third)
                    else:
                        text=doc.createTextNode(self.result[item1][item2])
                        second.appendChild(text)
                    first.appendChild(second)
            else:
                text=doc.createTextNode(self.result[item1])
                first.appendChild(text)
            result.appendChild(first)

        try:
            f=open(os.getcwd()+"/result.xml","w")
            doc.writexml(f,indent="   ",encoding="utf-8")
            f.close()
        except IOError,e:
            print str(e)


class XMLToHTML():
    '''this class is for converting xml to the html reports'''
    def __init__(self,path,compile_status_flag):
        self.xmldoc=minidom.parse(path)
        self.color=["#DCDCDC","#FF0000","#00FF00","#808A87"]
        self.compile_status_flag=compile_status_flag

    def getText(self,nodelist):
        text=""
        for node in nodelist:
            if node.nodeType == node.TEXT_NODE:
                text=text+node.data
        return text.strip()

    def xmlTohtml(self):
        saveout=sys.stdout
        f=open(os.getcwd()+"/result.html","w")
        sys.stdout=f
        print "<html>"
        print '<body bgcolor=%s>' % self.color[0]
        self.platformTohtml(self.xmldoc)
        print "<br />"
        self.prepareTohtml(self.xmldoc)
        print "<br />"
        if self.compile_status_flag == 'OK':
            self.testresultTohtml(self.xmldoc)
        else:
            print '<h1>Detail test results</h1>'
            print 'No dbus tests info!'
        print '<p><h3><b>Notes:</b></h3></p>'
        print '<p><font color=%s>Red:</font>regression <font color=%s>\
                  Green:</font>improvement <font color=%s>\
                  Gray:</font>failed but not regression</p>' % (self.color[1],self.color[2],self.color[3])
        print "</body>"
        print "</html>"
        sys.stdout=saveout
        f.close()

    def platformTohtml(self,dom):
        dom=dom.getElementsByTagName("platformInfo")[0]
        print "<div align=left valign=middle>",'<table border="2">'
        print "<caption>","<h2>","Platform Information","</h2>","</caption>"
        print '<tr><th>Item</th><th>Value</th></tr>'
        ElementNodes=[node for node in dom.childNodes if node.nodeType == node.ELEMENT_NODE]
        for node in ElementNodes:
            if node.nodeName == "libraryinfo":
                print "<tr><td>%s</td><td>%s</td></tr>" % (node.nodeName,self.getText(node.getElementsByTagName("libsoup")[0].childNodes)+" "+\
                       self.getText(node.getElementsByTagName("evolution-data-server")[0].childNodes)+" "+\
                       self.getText(node.getElementsByTagName("glib")[0].childNodes)+" "+\
                       self.getText(node.getElementsByTagName("dbus-glib")[0].childNodes))
            else:
                print "<tr><td>%s</td><td>%s</td></tr>" % (node.nodeName,self.getText(node.childNodes))
        print "</table>","</div>"

    def prepareTohtml(self,dom):
        dom=dom.getElementsByTagName("prepareResult")[0]
        print "<div align=left valign=middle>",'<table border="2">'
        print "<caption>","<h2>","Preparation Results","</h2>","</caption>"
        print '<tr><th>Prepare</th><th>Value</th></tr>'
        ElementNodes=[node for node in dom.childNodes if node.nodeType == node.ELEMENT_NODE]
        color=""
        for node in ElementNodes:
            if node.getElementsByTagName("today_status"):
                if self.getText(node.getElementsByTagName("today_status")[0].childNodes) == "OK":
                    if self.getText(node.getElementsByTagName("yesterday_status")[0].childNodes) == "FAIL":
                        color=self.color[2]
                    else:
                        color=self.color[0]
                else:
                    if self.getText(node.getElementsByTagName("yesterday_status")[0].childNodes) == "OK":
                        color=self.color[1]
                    else:
                        color=self.color[3]
                    
                print "<tr><td>%s</td><td bgcolor=%s><a href=%s>%s</a></td></tr>" % \
                    (node.nodeName,color,self.getText(node.getElementsByTagName("url")[0].childNodes),\
                    self.getText(node.getElementsByTagName("today_status")[0].childNodes))
            else:
                color=self.color[3]
                print "<tr><td>%s</td><td bgcolor=%s>%s</td></tr>" % (node.nodeName,color,self.getText(node.childNodes))
        print "</table>","</div>"

    def testresultTohtml(self,dom):
        self.summaryTohtml(dom)
        print "<br />"
        print '<h1>Detail test results</h1>'
        self.detailTohtml(dom)

    def summaryTohtml(self,dom):
        dom=dom.getElementsByTagName("testcases")[0]
        print "<div align=left valign=middle>",'<table border="2">'
        print "<caption>","<h2>""dbus summary tests results","</h2>","</caption>"
        ClassNodes=[node for node in dom.childNodes if node.nodeType == node.ELEMENT_NODE]
        totaltests=0
        okNumbers=0
        improvedNumbers=0
        regressionNumbers=0
        passrate=0
        improvedColor=self.color[0]
        regressionColor=self.color[0]
        improvedCases=[]
        regressionCases=[]
        for node in ClassNodes:
            totaltests+=int(node.getAttribute("ChildNumber"))
            TestcaseNodes=[testnode for testnode in node.childNodes if testnode.nodeType == testnode.ELEMENT_NODE]
            for testnode in TestcaseNodes:
                if self.getText(testnode.getElementsByTagName("today_status")[0].childNodes) == "OK":
                    okNumbers+=1
                    if self.getText(testnode.getElementsByTagName("yesterday_status")[0].childNodes) != "OK":
                        improvedNumbers+=1
                        improvedCases.append(node.nodeName+"."+testnode.nodeName)
                else:
                    if self.getText(testnode.getElementsByTagName("yesterday_status")[0].childNodes) == "OK":
                        regressionNumbers+=1
                        regressionCases.append(node.nodeName+"."+testnode.nodeName)
        if improvedNumbers != 0:
            improvedColor=self.color[2]
        if regressionNumbers != 0:
            regressionColor=self.color[1]
        if totaltests == 0:
            passrate=0
        else:
            passrate=float(okNumbers)/totaltests
        print '<tr><th>Total cases</th><th>Passed cases</th><th>Failed cases</th><th>Improved cases</th><th>Regression cases</th><th>Passrate</th></tr>'
        print '<tr><td>%s</td><td>%s</td><td>%s</td><td bgcolor=%s>%s</td><td bgcolor=%s>%s</td><td>%s</td></tr>' % \
               (totaltests,okNumbers,totaltests-okNumbers,improvedColor,improvedNumbers,regressionColor,regressionNumbers,passrate)
        print "</table>","</div>"

    def detailTohtml(self,dom):
        dom=dom.getElementsByTagName("testcases")[0]
        classCount=int(dom.getAttribute("ChildNumber"))
        ClassNodes=[node for node in dom.childNodes if node.nodeType == node.ELEMENT_NODE]
        for node in ClassNodes:
            print "<div align=left valign=middle>",'<table border="2">'
            print "<caption>","%s" % node.nodeName,"</caption>"
            print '<tr><th>Item</th><th>Value</th></tr>'
            okNumber=0
            TestcaseNodes=[testnode for testnode in node.childNodes if testnode.nodeType == testnode.ELEMENT_NODE]
            for testnode in TestcaseNodes:
                if self.getText(testnode.getElementsByTagName("today_status")[0].childNodes) == "OK":
                    okNumber+=1
                    if self.getText(testnode.getElementsByTagName("yesterday_status")[0].childNodes) == "OK":
                        color=self.color[0]
                    else:
                        color=self.color[2]
                else:
                    if self.getText(testnode.getElementsByTagName("yesterday_status")[0].childNodes) == "OK":
                        color=self.color[1]
                    else:
                        color=self.color[3]
                print '<tr><td>%s</td><td bgcolor=%s><a href=%s>%s</a></td></tr>' % \
                    (testnode.nodeName,color,self.getText(testnode.getElementsByTagName("url")[0].childNodes),\
                    self.getText(testnode.getElementsByTagName("today_status")[0].childNodes))
            print '<tr><td>total tests:  %s</td><td>fail test:  %s</td><tr>' % \
                 (node.getAttribute("ChildNumber"),int(node.getAttribute("ChildNumber"))-okNumber)
            print "</table>","</div>"

class MvToweb():
    def __init__(self):
        cmd='cp -rf %s %s ' % (today_result,webdir)
        runCommand(cmd)


class SendEmail():
    '''this class is for sending email to the specified users'''
    def __init__(self,authInfo,fromAdd,toAdd,subject,plainText,htmlText):
        self.server=authInfo.get('server')
        if not self.server:
            print "no mailhost info,exit now"
            sys.exit()
        self.plainText=plainText
        self.htmlText=htmlText
        self.msg=MIMEMultipart('related')
        self.msg['Subject']=subject
        self.msg['From']=fromAdd
        self.msg['To']=",".join(toAdd)
        self.msg.preamble='this is a multi-part message in mime format'
        
    def write(self):
        '''write the result reports to the object'''
        msgText=MIMEText(self.htmlText,'html','utf-8')
        self.msg.attach(msgText)
        '''fp=open("test.jpg","rb")
        msgImage=MIMEImage(fp.read())
        fp.close()
        msgImage.add_header("Content-ID","<image1>")
        self.msg.attach(msgImage)'''
        
    def send(self):
        '''send email '''
        try:
            smtp=smtplib.SMTP()
            smtp.set_debuglevel(0)
            smtp.connect(self.server)
            smtp.sendmail(self.msg['From'],self.msg['To'].split(","),self.msg.as_string())
            smtp.quit()
            return True
        except Exception,e:
            print str(e)
            return False



class TestMain():
    def __init__(self,args=None,compileflag='YES'):
        #self.clear()
        self.testcase=args
        self.compile_status_flag='OK'
        self.compileflag=compileflag
        self.USAGE=USAGE
        self.parseArgs()
        if self.compileflag.lower() == 'yes':
            self.get_make_source()
        if self.compile_status():
            self.get_testcasenames()
            if self.testcase == None:
                self.run_testcase(self.testcase)
            else:
                self.run_testcase(self.testcase[0])
        else:
            print "Error: Compiling in libsysnthesis or syncevolution"
        MvToweb()
        result=self.get_result(self.testcase,self.compile_status_flag)
        self.generate_xml(result)
        self.generate_html(self.compile_status_flag)
        self.send_email()
    def clear(self):
        cmd='current_date=`date +%Y%m%d`;rm -rf testCaseNames.txt testdbus.py testdbus.pyc'
        runCommand(cmd)

    def parseArgs(self):
        global srcpath
        global binpath
        global workpath
        global resultdir
        global testpath
        global webdir
        global server
        global today_result
        global yesterday_result
        global fromAdd
        global toAdd
        global mailhost
        global shell
        toAdd=[]
        shell=""
        argv=sys.argv
        try:
            options,args=getopt.getopt(argv[1:],'hHs',['help','recompile=','show','srcpath=','binpath=','workpath=','resultpath=','webpath=','webserver=','from=','to=','mailhost=', 'testpath=','shell='])
            for opt,value in options:
                if opt in ('-h','-H','--help'):
                    self.usageExit()
                if opt in ('--recompile'):
                    self.compileflag=value
                if opt in ('-s','--show'):
                    self.showTestCases()
                if opt in ('--srcpath'):
                    srcpath=abspath(value)
                if opt in ('--binpath'):
                    binpath=abspath(value)
                if opt in ('--workpath'):
                    workpath=abspath(value)
                if opt in ('--resultpath'):
                    resultdir=abspath(value)
                    today_result=resultdir+"/"+time.strftime("%Y%m%d",time.localtime())
                    yesterday_result=resultdir+"/"+''.join(str(datetime.datetime.now()+datetime.timedelta(days=-1)).split(' ')[0].split('-'))
                if opt in ('--webpath'):
                    webdir=abspath(value)
                if opt in ('--testpath'):
                    testpath=abspath(value)
                if opt in ('--webserver'):
                    server=value
                if opt in ('--from'):
                    fromAdd=value
                if opt in ('--to'):
                    toAdd.append(value)
                if opt in ('--mailhost'):
                    mailhost=value
                if opt in ('--shell'):
                    shell=value
            if not os.path.exists(today_result):
                os.makedirs(today_result)
            if not os.path.exists(webdir):
                os.makedirs(webdir)
            if len(args) != 0:
                self.testcase=args
        except getopt.error, msg:
            self.usageExit(msg)

    def usageExit(self,msg=None):
        if msg:
            print msg
        print self.USAGE
        sys.exit(2)

    def showTestCases(self):
        Tag="      --"
        try:
            f=open(workpath+"/testCaseNames.txt","r")
            Names=f.readlines()
            className1=None
            for name in Names:
                className=name.split('.')[0]
                testcaseName=name.split('.')[1]
                if className != className1:
                    print "-"+className
                className1=className
                print Tag+testcaseName,
            f.close()
        except IOError,e:
            print str(e)
        print self.USAGE
        sys.exit(2)

    def generate_html(self,compile_status_flag):
        htmlObj=XMLToHTML(os.getcwd()+"/result.xml",compile_status_flag)
        htmlObj.xmlTohtml()

    def generate_xml(self,result):
        xmlObj=ResultToXML(result)
        xmlObj.generateXML()

    def get_result(self,testcase,compile_status_flag):
        resultObj=GetResult(testcase,compile_status_flag)
        resultObj.getResult()
        return resultObj.result

    def run_testcase(self,testcase):
        runObj=RunTestcase(testcase)
        runObj.run()

    def get_testcasenames(self):
        namesObj=GetTestcaseNames()
        import testdbus
        namesObj.gettestcaseName(testdbus)
        namesObj.writeToFile()
    
    def compile_status(self):
        flag=True
        if os.path.isfile(today_result+"/compile_result.txt"):
            f=open(today_result+"/compile_result.txt","r")
            fetchLibS=f.readline().strip()
            compileLibS=f.readline().strip()
            fetchSync=f.readline().strip()
            compileSync=f.readline().strip()
            if compileLibS == 'FAIL' or compileSync == 'FAIL':
                flag=False
                self.compile_status_flag='FAIL'
            f.close()
        else:
            flag=False
            self.compile_status_flag='FAIL'
        return flag

    def get_make_source(self):
        sourceObj=GetSource()
        sourceObj.make()

    def send_email(self):
        authInfo={}
        authInfo['server']=mailhost
        subject="test-dbus testing report"
        plainText="the following is the testing result report for dubs-test"
        htmlText=""
        try:
            resulthtml=open("result.html","r")
            line=resulthtml.readline()
            while(line!=''):
                htmlText=htmlText+line
                line=resulthtml.readline()
            resulthtml.close()
        except IOError:
            htmlText='''<html><body><h1>Error:No HTML report generated!</h1></body></html>\n'''
        mailObj=SendEmail(authInfo,fromAdd,toAdd,subject,plainText,htmlText)
        mailObj.write()
        if mailObj.send():
            print "send successfully"
        else:
            print "send false"


if __name__=="__main__":
    TestMain()
