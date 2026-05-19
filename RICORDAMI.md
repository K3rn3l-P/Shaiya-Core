# Per aggiornare il main quando lo sviluppatore ufficiale fa nuovi commit:

### Scarica gli aggiornamenti ufficiali
```
git fetch upstream
git checkout main
git merge upstream/main
git push origin main
```
### Pusha il main aggiornato sul tuo repo privato
```
git push mio-repo main
```

### (opzionale) Ribase le tue modifiche sopra il nuovo main
```
git checkout modifiche
git rebase main
git push mio-repo modifiche --force-with-lease
```