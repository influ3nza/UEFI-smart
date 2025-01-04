VAR_PATH="/sys/firmware/efi/efivars/MyVariable-a3a56e56-1d23-06dc-24bf-1473ff54e629"
ATTRS="\x07\x00\x00\x00"

printf $ATTRS$1 > $VAR_PATH

if [ $? -ne 0 ]; then
    chattr -i $VAR_PATH
    printf $ATTRS$1 > $VAR_PATH
fi
