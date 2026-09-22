from pathlib import Path
import subprocess,tempfile,uuid,xml.etree.ElementTree as ET
import argparse
parser=argparse.ArgumentParser(description="Install an isolated per-user MSI fixture; verify config overwrite on upgrade and repair.")
parser.add_argument("--wix", required=True)
# WiX v7 refuses to build until its Open Source Maintenance Fee EULA is accepted
# (error WIX7015), once per machine or per invocation. As in installer\build_msi.ps1,
# this is opt-in and never accepted for you: read https://wixtoolset.org/osmf/ first.
parser.add_argument("--accept-wix-eula", action="store_true",
                    help="pass --acceptEula wix7 to wix for this run")
args=parser.parse_args()
eula=['--acceptEula','wix7'] if args.accept_wix_eula else []
repo=Path.cwd();d=Path(tempfile.mkdtemp(prefix='bigmap-config-msi-'));wix=Path(args.wix)
ns={'w':'http://wixtoolset.org/schemas/v4/wxs'}
source=ET.parse(repo/'installer/Package.wxs').find(".//w:Component[@Id='BigmapCfg']",ns)
assert 'NeverOverwrite' not in source.attrib
assert source.find('w:File',ns).get('DefaultVersion')=='$(ProductVersion)'
guid=str(uuid.uuid4());component=str(uuid.uuid4());dest=d/'installed';dest.mkdir()
config=repo/'cfg/tpf2_bigmap.cfg';expected=config.read_bytes()
for version,old in [('0.3.0',True),('0.3.1',False)]:
 flags=' NeverOverwrite="yes"' if old else ''
 fv='' if old else f' DefaultVersion="{version}"'
 xml=f'''<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs"><Package Name="Bigmap config overwrite test" Manufacturer="Local test" Version="{version}" UpgradeCode="{guid}" Scope="perUser"><MajorUpgrade DowngradeErrorMessage="Newer installed"/><MediaTemplate EmbedCab="yes"/><StandardDirectory Id="LocalAppDataFolder"><Directory Id="INSTALLDIR" Name="BigmapConfigTest"><Component Id="BigmapCfg" Guid="{component}"{flags}><File Id="Config" Name="tpf2_bigmap.cfg" Source="{config}" KeyPath="yes"{fv}/></Component></Directory></StandardDirectory><Feature Id="Main"><ComponentRef Id="BigmapCfg"/></Feature></Package></Wix>'''
 (d/f'{version}.wxs').write_text(xml)
 r=subprocess.run([str(wix),'build']+eula+['-arch','x64','-o',str(d/f'{version}.msi'),str(d/f'{version}.wxs')],capture_output=True,text=True)
 # Show wix's own diagnostics: swallowing them hid a WIX7015 EULA refusal as a
 # bare CalledProcessError.
 if r.returncode:
  raise SystemExit(f'wix build failed ({r.returncode}):\n{r.stdout}\n{r.stderr}')
def install(ver,*args):
 r=subprocess.run(['msiexec','/i',str(d/f'{ver}.msi'),'/qn',f'INSTALLDIR={dest}',*args],creationflags=subprocess.CREATE_NO_WINDOW)
 assert r.returncode in (0,3010),r.returncode
try:
 install('0.3.0')
 target=dest/'tpf2_bigmap.cfg';target.write_text('BROKEN USER CONFIG\noctree_depth=11\n')
 install('0.3.1');assert target.read_bytes()==expected,'upgrade did not overwrite'
 target.write_text('BROKEN AGAIN\n')
 install('0.3.1','REINSTALL=ALL','REINSTALLMODE=omus');assert target.read_bytes()==expected,'repair did not overwrite'
 print('PASS: actual MSI upgrade and repair overwrite modified configs:',d)
finally:
 subprocess.run(['msiexec','/x',str(d/'0.3.1.msi'),'/qn'],creationflags=subprocess.CREATE_NO_WINDOW)
